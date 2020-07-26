
#include "main.hpp"

#include "CmdLineArgs.hpp"
#include "common.hpp"
#include "grouping.hpp"
#include "version.h"

#include "htslib/bgzf.h"
#include "htslib/faidx.h"
#include "htslib/synced_bcf_reader.h"

#include <chrono>
#include <ctime>
#include <thread>
#include <tuple>

#if !defined(USE_STDLIB_THREAD)
#include "omp.h"
#endif

const unsigned int G_BLOCK_SIZE = 1000;

void 
xfree(void *ptr) {
    if (NULL != ptr) { free(ptr); }
}

template <bool TIsInputCompressed>
int 
clearstring(BGZF * bgzip_file, const std::string & outstring_allp, bool is_output_to_stdout = false, bool flush = !TIsInputCompressed) {
    if (is_output_to_stdout) {
        std::cout << outstring_allp;
        return outstring_allp.size();
    }
    if (NULL == bgzip_file) { return -1; }
    int ret = 0;
    if (TIsInputCompressed) {
        ret = bgzf_raw_write(bgzip_file, outstring_allp.c_str(), outstring_allp.size());
        LOG(logINFO) << "Written " << ret << " bytes of compressed data from " << outstring_allp.size()  << " bytes of compressed data.";
    } else {
        ret = bgzf_write(bgzip_file, outstring_allp.c_str(), outstring_allp.size());
        LOG(logINFO) << "Written " << ret << " bytes of compressed data from " << outstring_allp.size()  << " bytes of raw data.";
    }
    if (flush) { 
        int flushret = bgzf_flush(bgzip_file); 
        if (flushret != 0) {
            return flushret;
        }
    }
    return ret;
};

std::string 
load_refstring(const faidx_t *ref_faidx, unsigned int tid, unsigned int incbeg, unsigned int excend) {
    assert(incbeg < excend);
    if (NULL == ref_faidx) {
        return std::string(excend - incbeg, 'n');
    }
    const char *tname = faidx_iseq(ref_faidx, tid);
    int regionlen;
    char *fetchedseq = faidx_fetch_seq(ref_faidx, tname, incbeg, excend - 1, &regionlen);
    assert (regionlen == (int)(excend - incbeg) || !fprintf(stderr, "%d == %u - %u failed", regionlen, excend, incbeg));
    std::string ret(fetchedseq);
    for (size_t i = 0; i < ret.size(); i++) {
        ret[i] = toupper(ret[i]);
    }
    free(fetchedseq);
    return ret;
};

template <class TKey, class TVal>
std::vector<std::pair<TKey, TVal>>
map2vector(const std::map<TKey, TVal> & key2val4map) {
    std::vector<std::pair<TKey, TVal>> ret;
    for (auto kv : key2val4map) {
        ret.push_back(kv);
    }
    return ret;
};

std::map<std::pair<unsigned int, AlignmentSymbol>, std::set<size_t>>
mutform2count4vec_to_simplemut2indices(std::vector<std::pair<std::basic_string<std::pair<unsigned int, AlignmentSymbol>>, std::array<unsigned int, 2>>> mutform2count4vec) {
    std::map<std::pair<unsigned int, AlignmentSymbol>, std::set<size_t>> simplemut2indices;
    for (size_t i = 0; i < mutform2count4vec.size(); i++) {
        auto counts = mutform2count4vec[i].second; 
        if (counts[0] + counts[1] < 2) { continue; }
        std::basic_string<std::pair<unsigned int, AlignmentSymbol>> mutset = mutform2count4vec[i].first;
        for (auto simplemut : mutset) {
            simplemut2indices.insert(std::make_pair(simplemut, std::set<size_t>()));
            simplemut2indices[simplemut].insert(i);
            
            //for (std::pair<unsigned int, AlignmentSymbol> simplemut : mutforms) {
            //    simplemut2indices.insert(std::make_pair(simplemut, std::set<size_t>()));
            //    simplemut2indices[simplemut].insert(i);
            //}
        }
    }
    return simplemut2indices;
};

/*
std::map<unsigned int, std::set<size_t>> simplemut2indices
simplemut2indices4vec_to_pos2idx( mutform2count4vec) {
    std::map<unsigned int, std::set<size_t>> simplemut2indices;
    for (size_t i = 0; i < mutform2count4vec_bq.size(); i++) {
        for (auto mutform : mutform2count4vec_bq[i].first) {
            for (auto pos_symb : mutform) {
                unsigned int pos = pos_symb.first;
                if (pos_symb.second > 1) {
                    simplemut2indices.insert(std::make_pair(pos, std::set<size_t>()));
                    simplemut2indices[pos].insert(i);
                }
            }
        }
    }
}
*/

int 
bgzip_string(std::string & compressed_outstring, const std::string & uncompressed_outstring) {
    char *compressed_outstr = (char*)malloc(uncompressed_outstring.size() * sizeof(char));
    if (NULL == compressed_outstr) {
        fprintf(stderr, "The library function malloc failed at line %d in file %s !\n", __LINE__, __FILE__);
        exit(-1);
    }
    size_t compressed_capacity = uncompressed_outstring.size();
    size_t compressed_totlen = 0;
    size_t uncompress_totlen = 0;
    do {
        if (compressed_totlen + BGZF_BLOCK_SIZE >= compressed_capacity) {
            char *compressed_outstr_tmp = (char*)realloc(compressed_outstr, (compressed_capacity * 2 + BGZF_BLOCK_SIZE) * sizeof(char));
            if (NULL == compressed_outstr_tmp) {
                fprintf(stderr, "The library function realloc failed at line %d in file %s !\n", __LINE__, __FILE__);
                exit(-2);
            }
            compressed_outstr = compressed_outstr_tmp;
            compressed_capacity = compressed_capacity * 2 + BGZF_BLOCK_SIZE;
        }
        size_t block_len = MIN(uncompressed_outstring.size() - uncompress_totlen, (size_t)BGZF_BLOCK_SIZE);
        if (0 == block_len) { break; }
        size_t compressed_len = 0;
        bgzf_compress(compressed_outstr + compressed_totlen, &compressed_len, uncompressed_outstring.c_str() + uncompress_totlen, block_len, 5);
        uncompress_totlen += block_len;
        compressed_totlen += compressed_len;
        // fprintf(stderr, "Compressed %d out of %d raw data into %d\n", uncompressed_totlen, uncompressed_outstring.size(), compressed_totlen);
    } while (uncompress_totlen < uncompressed_outstring.size());
    compressed_outstring += std::string(compressed_outstr, compressed_totlen);
    free(compressed_outstr);
    return 0;
}

struct BatchArg {
    std::string outstring_allp;
    std::string outstring_pass;
    VcStats vc_stats;
    unsigned int thread_id;
    hts_idx_t *hts_idx;
    faidx_t *ref_faidx;
    bcf_hdr_t *bcf_hdr;
    bcf_srs_t *sr;
    
    std::tuple<unsigned int, unsigned int, unsigned int, bool, unsigned int> tid_beg_end_e2e_tuple;
    std::tuple<std::string, unsigned int> tname_tseqlen_tuple;
    unsigned int region_ordinal; // deprecated
    unsigned int region_tot_num; // deprecated
    unsigned int regionbatch_ordinal;
    unsigned int regionbatch_tot_num;

    const CommandLineArgs paramset;
    const std::string UMI_STRUCT_STRING;
    const bool is_vcf_out_pass_to_stdout;
    const bool is_vcf_out_empty_string;
};

unsigned int
gen_fq_tsum_depths(const auto & fq_tsum_depth, unsigned int refpos) {
    std::array<unsigned int, 3> fq_tsum_depths = {{0, 0, 0}};
    for (unsigned int strand = 0; strand < 2; strand++) {
        fq_tsum_depths[0] += fq_tsum_depth.at(strand).getByPos(refpos+0).sumBySymbolType(LINK_SYMBOL);
        fq_tsum_depths[1] += fq_tsum_depth.at(strand).getByPos(refpos+0).sumBySymbolType(BASE_SYMBOL);
        fq_tsum_depths[2] += fq_tsum_depth.at(strand).getByPos(refpos+1).sumBySymbolType(LINK_SYMBOL);
    }
    return MIN(MIN(fq_tsum_depths[0], fq_tsum_depths[2]), fq_tsum_depths[1]);
}

std::vector<unsigned int>
gen_dp100(const auto & fq_tsum_depth, unsigned int inclu_beg, unsigned int exclu_end) {
    assert (inclu_beg <= exclu_end);
    std::vector<unsigned int> dp100;
    dp100.reserve(exclu_end - inclu_beg);
    //for (unsigned int rpos2 = refpos; rpos2 < MIN(refpos+100, rpos_exclu_end); rpos2++) {
    for (unsigned int rpos2 = inclu_beg; rpos2 < exclu_end; rpos2++) {
        unsigned int fq_tsum_depth_g = gen_fq_tsum_depths(fq_tsum_depth, rpos2);
        dp100.push_back(fq_tsum_depth_g);
    }
    return dp100;
}

std::string 
dp100_to_string(const std::vector<unsigned int> & bg1dp100, const std::vector<unsigned int> & bg2dp100, 
        const std::string & chromosome, unsigned int refpos, bool is_rev) {
    assert (bg1dp100.size() == bg2dp100.size());
    std::string ret = chromosome + "\t" + std::to_string(refpos) + 
            "\t.\tN\t<DP100" + (is_rev ? "RV" : "FW") + ">\t.\t.\t.\tGT:bgNPOS:bg1DPS:bg2DPS\t.:" + std::to_string(bg1dp100.size()) + ":";
    for (auto dp : bg1dp100) {
        ret += std::to_string(dp) + ",";
    }
    ret += "-1:";
    for (auto dp : bg2dp100) {
        ret += std::to_string(dp) + ",";
    }
    ret += "-1\n";
    return ret;
}

std::string
genomic_reg_info_to_string(const std::string & chromosome, 
        unsigned int incluBeg, SymbolType stypeBeg,
        unsigned int incluEnd, SymbolType stypeEnd,
        const unsigned int gbDPmin, const unsigned int gcDPmin,
        const std::string &gfGTmm2, const unsigned int gfGQmin,
        const std::string & refstring, unsigned int refstring_offset) {
    unsigned int begpos = incluBeg; // = (stypeBeg == BASE_SYMBOL ? (incluBeg+1) : incluBeg);
    unsigned int endpos = incluEnd; // = (stypeEnd == BASE_SYMBOL ? (excluEnd+1) : (excluEnd));
    unsigned int refstring_idx = begpos - refstring_offset;
    const std::string begchar = (refstring_idx > 0 ? refstring.substr(refstring_idx - 1, 1) : "n");
    std::string ret = chromosome + "\t" + std::to_string(begpos)
            + "\t.\t" + begchar + "\t<NON_REF" + ">\t.\t.\t.\tGT:GQ:gbDP:gcDP:gSTS:gBEG:gEND\t"
            +               (gfGTmm2) + ":"
            + std::to_string(gfGQmin) + ":"
            + std::to_string(gbDPmin) + ":"
            + std::to_string(gcDPmin) + ":"
            + std::to_string(stypeBeg) + "," + std::to_string(stypeEnd) + ":"
            + std::to_string(begpos) + ":"
            + std::to_string(endpos) + "\n";
    return ret;
}

const bool
is_sig_higher_than(auto a, auto b, unsigned int mfact, unsigned int afact) {
    return (a * 100 > b * (100 + mfact)) && (a > b + afact);
}

const bool
is_sig_out(auto a, auto minval, auto maxval, unsigned int mfact, unsigned int afact) {
    return is_sig_higher_than(a, minval, mfact, afact) || is_sig_higher_than(maxval, a, mfact, afact);
}

struct TumorKeyInfo {
    std::string ref_alt;
    std::string FTS;
    int32_t pos = 0;
    float VAQ = 0;
    int32_t DP = 0;
    float FA = 0;
    float FR = 0;
    int32_t BQ = 0;
    int32_t MQ = 0;
   
    int32_t cADTC = 0;
    int32_t cRDTC = 0;
    int32_t cDPTC = 0;
    
    int32_t bDP = 0;
    std::array<int32_t, 2> bAD1 = {{0}};
    int32_t autoBestAllBQ = 0;
    int32_t autoBestAltBQ = 0;
    int32_t autoBestRefBQ = 0;
    int32_t autoBestAllHD = 0;
    int32_t autoBestAltHD = 0;
    int32_t autoBestRefHD = 0;
    int32_t cAllBQ = 0;
    int32_t cAltBQ = 0;
    int32_t cRefBQ = 0;
    int32_t dAD3 = 0;
    std::array<int32_t, 4> gapDP4 = {{0}};
    std::array<int32_t, 6*RCC_NUM> RCC = {{0}};
    std::array<int32_t, 3> GLa = {{0}};
    std::array<int32_t, 3> GLb = {{0}};
    std::array<int32_t, 5> EROR = {{0}};
    std::array<int32_t, 4> gapbNRD = {{0}};
    std::array<int32_t, 3> aPBAD = {{0}};
    std::array<int32_t, 4> aAD = {{0}};
    // std::array<int32_t, 2> gapbNNRD = {0};
    bcf1_t *bcf1_record = NULL;
    /*
    ~TumorKeyInfo() {
        if (bcf1_record != NULL) {
            // this line must be elsewhere apparently due to the subtle differences between C and C++.
            // bcf_destroy(bcf1_record);
        }
    }
    */
};

std::string 
als_to_string(const char *const* const allele, unsigned int m_allele) {
    std::string ret;
    ret.reserve(m_allele*2);
    for (unsigned int i = 0; i < m_allele; i++) {
        if (0 == i) {
            ret += allele[i];
        } else if (1 == i) {
            ret += std::string("\t") + allele[i];
        } else {
            ret += std::string(",") + allele[i];
        }
    }
    /*
    ret.reserve(m_als+1);
    unsigned int n_alleles = 0;
    for (unsigned int i = 0; i < m_als; i++) {
        if ('\0' == als[i]) {
            if (0 == n_alleles) {
                ret += "\t";
            } else if ((i+1) != m_als) {
                ret += ",";
            } else {
                // pass
            }
            n_alleles++;
        } else {
            ret += als[i];
        }
    }
    */
    return ret;
}

const std::map<std::tuple<unsigned int, unsigned int, AlignmentSymbol>, std::vector<TumorKeyInfo>>
rescue_variants_from_vcf(const auto & tid_beg_end_e2e_vec, const auto & tid_to_tname_tlen_tuple_vec, const std::string & vcf_tumor_fname, const auto *bcf_hdr, 
        const bool is_tumor_format_retrieved) {
    std::map<std::tuple<unsigned int, unsigned int, AlignmentSymbol>, std::vector<TumorKeyInfo>> ret;
    if (NOT_PROVIDED == vcf_tumor_fname) {
        return ret;
    }
    std::string regionstring;
    for (const auto & tid_beg_end_e2e : tid_beg_end_e2e_vec) {
        const unsigned int tid = std::get<0>(tid_beg_end_e2e);
        const unsigned int rpos_inclu_beg = std::get<1>(tid_beg_end_e2e);
        const unsigned int rpos_exclu_end = std::get<2>(tid_beg_end_e2e);
        
        const auto & tname_tseqlen_tuple = tid_to_tname_tlen_tuple_vec[tid];
        if (0 < regionstring.size()) { regionstring += std::string(","); }
        regionstring += (std::get<0>(tname_tseqlen_tuple) + ":" + std::to_string(rpos_inclu_beg+1) + "-" + std::to_string(rpos_exclu_end+1-1)); 
    }
    
    { 
        LOG(logINFO) << "Region is " << regionstring;
    }
    if (regionstring.size() == 0) {
        return ret;
    }
    bcf_srs_t *const sr = bcf_sr_init();
    if (NULL == sr) {
        LOG(logCRITICAL) << "Failed to initialize bcf sr";
        exit(-6);
    }
    
    // int retflag = // bcf_sr_set_opt(srs[i], BCF_SR_PAIR_LOGIC, BCF_SR_PAIR_BOTH_REF); 
    bcf_sr_set_regions(sr, regionstring.c_str(), false);
    /*int sr_set_opt_retval =*/ 
    bcf_sr_set_opt(sr, BCF_SR_REQUIRE_IDX);
    int sr_add_reader_retval = bcf_sr_add_reader(sr, vcf_tumor_fname.c_str());
    if (sr_add_reader_retval != 1) {
        LOG(logCRITICAL) << "Failed to synchronize-read the tumor vcf " << vcf_tumor_fname << " with return code " << sr_add_reader_retval;
        exit(-7);
    }
    
    int valsize = 0; 
    int ndst_val = 0;
    char *bcfstring = NULL;
    float *bcffloats = NULL;
    int32_t *bcfints = NULL;
    
    while (bcf_sr_next_line(sr)) {
        bcf1_t *line = bcf_sr_get_line(sr, 0);
        bcf_unpack(line, BCF_UN_ALL);
        
        // skip over all symbolic alleles
        bool should_continue = false;
        for (unsigned int i = 1; i < line->n_allele; i++) {
            if ('<' == line->d.allele[i][0]) {
                should_continue = true;
            }
        }
        if (should_continue) { continue; }
        ndst_val = 0;
        valsize = bcf_get_format_char(bcf_hdr, line, "VType", &bcfstring, &ndst_val);
        if (valsize <= 0) { continue; }
        assert(ndst_val == valsize);
        assert(2 == line->n_allele || !fprintf(stderr, "Bcf line %d has %d alleles!\n", line->pos, line->n_allele));
        std::string desc(bcfstring);
        // LOG(logINFO) << "Trying to retrieve the symbol " << desc << " at pos " << line->pos << " valsize = " << valsize << " ndst_val = " << ndst_val;
        AlignmentSymbol symbol = DESC_TO_SYMBOL_MAP.at(desc);
        
        auto symbolpos = (isSymbolSubstitution(symbol) ? (line->pos) : (line->pos + 1));
        /*
        if (symbolpos < extended_inclu_beg_pos && isSymbolDel(symbol)) {
            continue;
        }
        auto intpos = symbolpos - extended_inclu_beg_pos;
        if (!(intpos < extended_posidx_to_symbol_to_tkinfo.size())) {
            LOG(logINFO) << "Warning!!! Trying to retrieve the symbol " << desc << " at pos " << line->pos << " valsize = " << valsize << " ndst_val = " << ndst_val;
            fprintf(stderr, "%d < %d failed with regionstring %s!\n", intpos, extended_posidx_to_symbol_to_tkinfo.size(), regionstring.c_str());
            abort();
        }
        */
        TumorKeyInfo tki;
        
        ndst_val = 0;
        valsize = bcf_get_format_int32(bcf_hdr, line, "VAQ", &bcfints, &ndst_val);
        assert((1 == ndst_val && 1 == valsize) || !fprintf(stderr, "1 == %d && 1 == %d failed for VAQ and line %d!\n", ndst_val, valsize, line->pos));
        tki.VAQ = bcfints[0];

        ndst_val = 0;
        valsize = bcf_get_format_float(bcf_hdr, line,  "FA", &bcffloats,  &ndst_val);
        assert((1 == ndst_val && 1 == valsize) || !fprintf(stderr, "1 == %d && 1 == %d failed for FA!\n", ndst_val, valsize));
        tki.FA = bcffloats[0];

        ndst_val = 0;
        valsize = bcf_get_format_float(bcf_hdr, line,  "FR", &bcffloats,  &ndst_val);
        assert((1 == ndst_val && 1 == valsize) || !fprintf(stderr, "1 == %d && 1 == %d failed for FR!\n", ndst_val, valsize)); 
        tki.FR = bcffloats[0];
        
        ndst_val = 0;
        valsize = bcf_get_format_int32(bcf_hdr, line,  "BQ", &bcfints,  &ndst_val);
        assert((1 == ndst_val && 1 == valsize) || !fprintf(stderr, "1 == %d && 1 == %d failed for BQ!\n", ndst_val, valsize));
        tki.BQ = bcfints[0];
        
        ndst_val = 0;
        valsize = bcf_get_format_int32(bcf_hdr, line,  "MQ", &bcfints,  &ndst_val);
        assert((1 == ndst_val && 1 == valsize) || !fprintf(stderr, "1 == %d && 1 == %d failed for MQ!\n", ndst_val, valsize)); 
        tki.MQ = bcfints[0];
        

        ndst_val = 0;
        valsize = bcf_get_format_int32(bcf_hdr, line,  "cADTC", &bcfints,  &ndst_val);
        assert((2 == ndst_val && 2 == valsize) || !fprintf(stderr, "2 == %d && 2 == %d failed for cADTC!\n", ndst_val, valsize));
        tki.cADTC = bcfints[0] + bcfints[1];
        
        ndst_val = 0;
        valsize = bcf_get_format_int32(bcf_hdr, line,  "cRDTC", &bcfints,  &ndst_val);
        assert((2 == ndst_val && 2 == valsize) || !fprintf(stderr, "2 == %d && 2 == %d failed for cRDTC!\n", ndst_val, valsize));
        tki.cRDTC = bcfints[0] + bcfints[1];
        
        ndst_val = 0;
        valsize = bcf_get_format_int32(bcf_hdr, line,  "cDPTC", &bcfints,  &ndst_val);
        assert((2 == ndst_val && 2 == valsize) || !fprintf(stderr, "2 == %d && 2 == %d failed for cDPTC!\n", ndst_val, valsize));
        tki.cDPTC = bcfints[0] + bcfints[1];

        ndst_val = 0;
        valsize = bcf_get_format_int32(bcf_hdr, line,  "DP", &bcfints,  &ndst_val);
        assert((1 == ndst_val && 1 == valsize) || !fprintf(stderr, "1 == %d && 1 == %d failed for DP!\n", ndst_val, valsize));
        tki.DP = bcfints[0];
        
        ndst_val = 0;
        valsize = bcf_get_format_int32(bcf_hdr, line,  "bDP", &bcfints,  &ndst_val);
        assert((1 == ndst_val && 1 == valsize) || !fprintf(stderr, "1 == %d && 1 == %d failed for DP!\n", ndst_val, valsize));
        tki.bDP = bcfints[0]; 
        
        ndst_val = 0;
        valsize = bcf_get_format_int32(bcf_hdr, line,  "cAllBQ", &bcfints, &ndst_val);
        assert((2 == ndst_val && 2 == valsize) || !fprintf(stderr, "2 == %d && 2 == %d failed for cAllBQ!\n", ndst_val, valsize));
        tki.autoBestAllBQ = bcfints[0] + bcfints[1];
        
        ndst_val = 0;
        valsize = bcf_get_format_int32(bcf_hdr, line,  "cAltBQ", &bcfints, &ndst_val);
        assert((2 == ndst_val && 2 == valsize) || !fprintf(stderr, "2 == %d && 2 == %d failed for cAltBQ!\n", ndst_val, valsize));
        tki.autoBestAltBQ = bcfints[0] + bcfints[1];
        
        ndst_val = 0;
        valsize = bcf_get_format_int32(bcf_hdr, line,  "cRefBQ", &bcfints, &ndst_val);
        assert((2 == ndst_val && 2 == valsize) || !fprintf(stderr, "2 == %d && 2 == %d failed for cRefBQ!\n", ndst_val, valsize));
        tki.autoBestRefBQ = bcfints[0] + bcfints[1];
        
        ndst_val = 0;
        valsize = bcf_get_format_int32(bcf_hdr, line,  "cAllHD", &bcfints, &ndst_val);
        assert((2 == ndst_val && 2 == valsize) || !fprintf(stderr, "2 == %d && 2 == %d failed for cAllHD!\n", ndst_val, valsize));
        tki.autoBestAllHD = bcfints[0] + bcfints[1];

        ndst_val = 0;
        valsize = bcf_get_format_int32(bcf_hdr, line,  "cAltHD", &bcfints, &ndst_val);
        assert((2 == ndst_val && 2 == valsize) || !fprintf(stderr, "2 == %d && 2 == %d failed for cAltHD!\n", ndst_val, valsize));
        tki.autoBestAltHD = bcfints[0] + bcfints[1];
        
        ndst_val = 0;
        valsize = bcf_get_format_int32(bcf_hdr, line,  "cRefHD", &bcfints, &ndst_val);
        assert((2 == ndst_val && 2 == valsize) || !fprintf(stderr, "2 == %d && 2 == %d failed for cRefHD!\n", ndst_val, valsize));
        tki.autoBestRefHD = bcfints[0] + bcfints[1];
        
        ndst_val = 0;
        valsize = bcf_get_format_int32(bcf_hdr, line,  "cAllBQ", &bcfints, &ndst_val);
        assert((2 == ndst_val && 2 == valsize) || !fprintf(stderr, "2 == %d && 2 == %d failed for cAllBQ!\n", ndst_val, valsize));
        tki.cAllBQ = bcfints[0] + bcfints[1];
        
        ndst_val = 0;
        valsize = bcf_get_format_int32(bcf_hdr, line,  "cAltBQ", &bcfints, &ndst_val);
        assert((2 == ndst_val && 2 == valsize) || !fprintf(stderr, "2 == %d && 2 == %d failed for cAltBQ2!\n", ndst_val, valsize));
        tki.cAltBQ = bcfints[0] + bcfints[1];
        
        ndst_val = 0;
        valsize = bcf_get_format_int32(bcf_hdr, line,  "cRefBQ", &bcfints, &ndst_val);
        assert((2 == ndst_val && 2 == valsize) || !fprintf(stderr, "2 == %d && 2 == %d failed for cRefBQ!\n", ndst_val, valsize));
        tki.cRefBQ = bcfints[0] + bcfints[1];
        
        ndst_val = 0;
        valsize = bcf_get_format_int32(bcf_hdr, line,  "dAD3", &bcfints, &ndst_val);
        assert((1 == ndst_val && 1 == valsize) || !fprintf(stderr, "1 == %d && 1 == %d failed for dAD3!\n", ndst_val, valsize));
        tki.dAD3 = bcfints[0];
        
        ndst_val = 0;
        valsize = bcf_get_format_char(bcf_hdr, line, "FTS", &bcfstring, &ndst_val);
        assert((ndst_val == valsize && 0 < valsize) || !fprintf(stderr, "%d == %d && nonzero failed for FTS!\n", ndst_val, valsize));
        /*
        for (int ftidx = 0; ftidx < valsize - 1; ftidx++) {
            if (';' == bcfstring[ftidx]) {
                bcfstring[ftidx] = '.';
            }
        }
        */
        tki.FTS = std::string(bcfstring, valsize-1);
        
        ndst_val = 0;
        valsize = bcf_get_format_int32(bcf_hdr, line,  "gapDP4", &bcfints, &ndst_val);
        assert((tki.gapDP4.size() == ndst_val && tki.gapDP4.size() == valsize) || !fprintf(stderr, "%lu == %d && %lu == %d failed for gapDP4!\n", tki.gapDP4.size(), ndst_val, tki.gapDP4.size(), valsize));
        for (size_t i = 0; i < tki.gapDP4.size(); i++) {
            tki.gapDP4[i] = bcfints[i];
        }
        ndst_val = 0;
        valsize = bcf_get_format_int32(bcf_hdr, line,  "RCC", &bcfints, &ndst_val);
        assert((tki.RCC.size() == ndst_val && tki.RCC.size() == valsize) || !fprintf(stderr, "%lu == %d && %lu == %d failed for RCC!\n", tki.RCC.size(), ndst_val, tki.RCC.size(), valsize));
        for (size_t i = 0; i < tki.RCC.size(); i++) {
            tki.RCC[i] = bcfints[i];
        }
        
        ndst_val = 0;
        valsize = bcf_get_format_int32(bcf_hdr, line,  "GLa", &bcfints, &ndst_val);
        assert((tki.GLa.size() == ndst_val && tki.GLa.size() == valsize) || !fprintf(stderr, "%lu == %d && %lu == %d failed for GQa!\n", tki.GLa.size(), ndst_val, tki.GLa.size(), valsize));
        for (size_t i = 0; i < tki.GLa.size(); i++) {
            tki.GLa[i] = bcfints[i];
        }
        
        ndst_val = 0;
        valsize = bcf_get_format_int32(bcf_hdr, line,  "GLb", &bcfints, &ndst_val);
        assert((tki.GLb.size() == ndst_val && tki.GLb.size() == valsize) || !fprintf(stderr, "%lu == %d && %lu == %d failed for GQb!\n", tki.GLb.size(), ndst_val, tki.GLb.size(), valsize));
        for (size_t i = 0; i < tki.GLb.size(); i++) {
            tki.GLb[i] = bcfints[i];
        }
        
        ndst_val = 0;
        valsize = bcf_get_format_int32(bcf_hdr, line,  "EROR",  &bcfints, &ndst_val);
        assert((tki.EROR.size() == ndst_val && tki.EROR.size() == valsize) || !fprintf(stderr, "%lu == %d && %lu == %d failed for EROR!\n", tki.EROR.size(), ndst_val, tki.EROR.size(), valsize));
        for (size_t i = 0; i < tki.EROR.size(); i++) {
            tki.EROR[i] = bcfints[i];
        }
        
        ndst_val = 0;
        valsize = bcf_get_format_int32(bcf_hdr, line,  "gapbNRD",&bcfints,&ndst_val);
        assert((tki.gapbNRD.size() == ndst_val && tki.gapbNRD.size() == valsize) || !fprintf(stderr, "%lu == %d && %lu == %d failed for gapbNRD!\n", tki.gapbNRD.size(), ndst_val, tki.gapbNRD.size(), valsize));
        for (size_t i = 0; i < tki.gapbNRD.size(); i++) {
            tki.gapbNRD[i] = bcfints[i];
        }
        
        ndst_val = 0;
        valsize = bcf_get_format_int32(bcf_hdr, line,  "aPBAD",&bcfints,&ndst_val);
        assert((tki.aPBAD.size() == ndst_val && tki.aPBAD.size() == valsize) || !fprintf(stderr, "%lu == %d && %lu == %d failed for aPBAD!\n", tki.aPBAD.size(), ndst_val, tki.aPBAD.size(), valsize));
        for (size_t i = 0; i < tki.aPBAD.size(); i++) {
            tki.aPBAD[i] = bcfints[i];
        }
        
        ndst_val = 0;
        valsize = bcf_get_format_int32(bcf_hdr, line,  "aAD",&bcfints,&ndst_val);
        assert((tki.aAD.size() == ndst_val && tki.aAD.size() == valsize) || !fprintf(stderr, "%lu == %d && %lu == %d failed for aAD!\n", tki.aAD.size(), ndst_val, tki.aAD.size(), valsize));
        for (size_t i = 0; i < tki.aAD.size(); i++) {
            tki.aAD[i] = bcfints[i];
        }
        
        /*
        ndst_val = 0;
        valsize = bcf_get_format_int32(bcf_hdr, line,  "gapbNNRD",&bcfints,&ndst_val);
        assert((tki.gapbNNRD.size() == ndst_val && tki.gapbNNRD.size() == valsize) || !fprintf(stderr, "%lu == %d && %lu == %d failed for gapbNNRD!\n", tki.gapbNNRD.size(), ndst_val, tki.gapbNNRD.size(), valsize));
        for (size_t i = 0; i < tki.gapbNNRD.size(); i++) {
            tki.gapbNNRD[i] = bcfints[i];
        }
        */
        
        ndst_val = 0;
        valsize = bcf_get_format_int32(bcf_hdr, line,  "bAD1",  &bcfints, &ndst_val);
        assert((tki.bAD1.size() == ndst_val && tki.bAD1.size() == valsize) || !fprintf(stderr, "%lu == %d && %lu == %d failed for gapbNRD!\n", tki.bAD1.size(), ndst_val, tki.bAD1.size(), valsize));
        for (size_t i = 0; i < tki.bAD1.size(); i++) {
            tki.bAD1[i] = bcfints[i];
        }
        
        ndst_val = 0;
        valsize = bcf_get_format_char(bcf_hdr, line, "cHap", &bcfstring, &ndst_val);
        assert((ndst_val == valsize && 0 < valsize) || !fprintf(stderr, "%d == %d && nonzero failed for cHap!\n", ndst_val, valsize));
        tki.FTS += std::string(";tcHap=") + std::string(bcfstring);
        
        tki.pos = line->pos;
        tki.ref_alt = als_to_string(line->d.allele, line->n_allele);
        if (is_tumor_format_retrieved) {
            tki.bcf1_record = bcf_dup(line);
        }
        const auto retkey = std::make_tuple(line->rid, symbolpos, symbol);            
        ret.insert(std::make_pair(retkey, std::vector<TumorKeyInfo>()));
        ret[retkey].push_back(tki);
        // ret.insert(std::make_pair(retkey, tki));
    }
    xfree(bcfstring);
    xfree(bcfints);
    xfree(bcffloats);
    bcf_sr_destroy(sr);
    return ret;
}

int 
process_batch(BatchArg & arg, const auto & tid_pos_symb_to_tkis) {
    
    std::string & outstring_allp = arg.outstring_allp;
    std::string & outstring_pass = arg.outstring_pass;
    const hts_idx_t *const hts_idx = arg.hts_idx;
    const faidx_t *const ref_faidx = arg.ref_faidx;
    
    const bcf_hdr_t *const bcf_hdr = arg.bcf_hdr;
    // bcf_srs_t *const sr = arg.sr;
    
    const CommandLineArgs & paramset = arg.paramset;
    const std::string UMI_STRUCT_STRING = arg.UMI_STRUCT_STRING;
    const std::tuple<unsigned int, unsigned int, unsigned int, bool, unsigned int> tid_beg_end_e2e_tuple = arg.tid_beg_end_e2e_tuple;
    const std::tuple<std::string, unsigned int> tname_tseqlen_tuple = arg.tname_tseqlen_tuple;
    //const unsigned int region_ordinal = arg.region_ordinal;
    //const unsigned int region_tot_num = arg.region_tot_num;
    const unsigned int regionbatch_ordinal = arg.regionbatch_ordinal;
    const unsigned int regionbatch_tot_num = arg.regionbatch_tot_num;
    const unsigned int thread_id = arg.thread_id;
    const bool should_output_all = !arg.is_vcf_out_empty_string;
    const bool should_let_all_pass = paramset.should_let_all_pass;
    const bool is_vcf_out_pass_to_stdout = arg.is_vcf_out_pass_to_stdout;
    
    bool is_loginfo_enabled = (ispowerof2(regionbatch_ordinal + 1) || ispowerof2(regionbatch_tot_num - regionbatch_ordinal));
    std::string raw_out_string;
    std::string raw_out_string_pass;
    // faidx_t *ref_faidx = (fasta_ref_fname.size() > 0 ? fai_load(fasta_ref_fname.c_str()) : NULL);
    
    auto tid = std::get<0>(tid_beg_end_e2e_tuple);
    auto incluBegPosition = std::get<1>(tid_beg_end_e2e_tuple);
    auto excluEndPosition = std::get<2>(tid_beg_end_e2e_tuple);
    auto end2end = std::get<3>(tid_beg_end_e2e_tuple);
    // auto nreads = std::get<4>(tid_beg_end_e2e_tuple);
    
    std::map<uint64_t, std::pair<std::array<std::map<uint64_t, std::vector<bam1_t *>>, 2>, int>> umi_to_strand_to_reads;
    unsigned int extended_inclu_beg_pos, extended_exclu_end_pos; 
    std::vector<std::pair<std::array<std::vector<std::vector<bam1_t *>>, 2>, int>> umi_strand_readset;

    if (is_loginfo_enabled) { LOG(logINFO) << "Thread " << thread_id << " starts bamfname_to_strand_to_familyuid_to_reads with pair_end_merge = " << paramset.pair_end_merge; }
    std::array<unsigned int, 3> passed_pcrpassed_umipassed = bamfname_to_strand_to_familyuid_to_reads(
            umi_to_strand_to_reads,
            extended_inclu_beg_pos,
            extended_exclu_end_pos,
            paramset.bam_input_fname,
            tid,
            incluBegPosition, 
            excluEndPosition,
            end2end,
            paramset.min_mapqual,
            paramset.min_aln_len,
            regionbatch_ordinal,
            regionbatch_tot_num,
            UMI_STRUCT_STRING,
            hts_idx,
            ASSAY_TYPE_CAPTURE != paramset.assay_type,
            PAIR_END_MERGE_NO != paramset.pair_end_merge,
            paramset.disable_duplex,
            thread_id,
            paramset.dedup_center_mult,
            paramset.dedup_amplicon_count_to_surrcount_ratio,
            paramset.dedup_amplicon_end2end_ratio,
            paramset.always_log,
            (SEQUENCING_PLATFORM_IONTORRENT == paramset.sequencing_platform),
            paramset.dedup_flag,
            0);
    
    unsigned int num_passed_reads = passed_pcrpassed_umipassed[0];
    unsigned int num_pcrpassed_reads = passed_pcrpassed_umipassed[1];
    unsigned int num_umipassed_reads = passed_pcrpassed_umipassed[2];
    bool is_umi_barcoded = (num_umipassed_reads * 2 > num_passed_reads);
    bool is_by_capture = ((num_pcrpassed_reads) * 2 <= num_passed_reads);
    const AssayType inferred_assay_type = ((ASSAY_TYPE_AUTO == paramset.assay_type) ? (is_by_capture ? ASSAY_TYPE_CAPTURE : ASSAY_TYPE_AMPLICON) : (paramset.assay_type));
    
    if (0 == num_passed_reads) { return -1; };
    unsigned int minABQ_snv = ((ASSAY_TYPE_AMPLICON == inferred_assay_type) ? paramset.minABQ_pcr_snv : paramset.minABQ_cap_snv);
    unsigned int minABQ_indel = ((ASSAY_TYPE_AMPLICON == inferred_assay_type) ? paramset.minABQ_pcr_indel : paramset.minABQ_cap_indel);
    
    const unsigned int rpos_inclu_beg = MAX(incluBegPosition, extended_inclu_beg_pos);
    const unsigned int rpos_exclu_end = MIN(excluEndPosition, extended_exclu_end_pos); 

    //std::vector<std::array<TumorKeyInfo, NUM_ALIGNMENT_SYMBOLS>> extended_posidx_to_symbol_to_tkinfo(extended_exclu_end_pos - extended_inclu_beg_pos + 1);
    const auto tkis_beg = tid_pos_symb_to_tkis.lower_bound(std::make_tuple(tid, extended_inclu_beg_pos    , AlignmentSymbol(0)));
    const auto tkis_end = tid_pos_symb_to_tkis.upper_bound(std::make_tuple(tid, extended_exclu_end_pos + 1, AlignmentSymbol(0)));
    std::vector<bool> extended_posidx_to_is_rescued(extended_exclu_end_pos - extended_inclu_beg_pos + 1, false);
    unsigned int num_rescued = 0;
    for (auto tkis_it = tkis_beg; tkis_it != tkis_end; tkis_it++) {
        auto symbolpos = std::get<1>(tkis_it->first);
        extended_posidx_to_is_rescued[symbolpos - extended_inclu_beg_pos] = true;
        num_rescued++;
        if (is_loginfo_enabled) {
            // NOTE: the true positive short del at 22:17946835 in NA12878-NA24385 mixture is overwhelmed by the false positve long del spanning the true positive short del.
            // However, so far manual check with limited experience cannot confirm that the true positive is indeed a true positive.
            // TODO: have to see more examples of this case and adjust code accordingly.
            LOG(logDEBUG4) << "Thread " << thread_id << " iterated over symbolpos " << symbolpos << " and symbol " << std::get<2>(tkis_it->first) << " as a rescued var";
        }
    }
    if (is_loginfo_enabled) { LOG(logINFO) << "Thread " << thread_id << " deals with " << num_rescued << " tumor-sample variants in region " << extended_inclu_beg_pos << " to " << extended_exclu_end_pos + 1 ;}
    // auto tki_it = tki_beg; 
    //if (paramset.vcf_tumor_fname.size() != 0) {
        // do not check tumor vcf here.
    //}
    
    if (is_loginfo_enabled) { LOG(logINFO) << "Thread " << thread_id << " starts converting umi_to_strand_to_reads with is_by_capture = " << is_by_capture << "  " ;}
    fill_strand_umi_readset_with_strand_to_umi_to_reads(umi_strand_readset, umi_to_strand_to_reads, paramset.baq_per_aligned_base);
    
    if (is_loginfo_enabled) { LOG(logINFO) << "Thread " << thread_id << " starts constructing symbolToCountCoverageSet12 with " << extended_inclu_beg_pos << (" , ") << extended_exclu_end_pos; }
    // + 1 accounts for insertion at the end of the region, this should happen rarely for only re-aligned reads at around once per one billion base pairs
    Symbol2CountCoverageSet symbolToCountCoverageSet12(tid, extended_inclu_beg_pos, extended_exclu_end_pos + 1); 
    if (is_loginfo_enabled) { LOG(logINFO)<< "Thread " << thread_id << " starts updateByRegion3Aln with " << umi_strand_readset.size() << " families"; }
    std::string refstring = load_refstring(ref_faidx, tid, extended_inclu_beg_pos, extended_exclu_end_pos);
    std::vector<RegionalTandemRepeat> region_repeatvec = refstring2repeatvec(refstring);
    // repeatvec_LOG(region_repeatvec, extended_inclu_beg_pos);

    std::vector<std::tuple<unsigned int, unsigned int, unsigned int>> adjcount_x_rpos_x_misma_vec;
    std::map<std::basic_string<std::pair<unsigned int, AlignmentSymbol>>, std::array<unsigned int, 2>> mutform2count4map_bq;
    std::map<std::basic_string<std::pair<unsigned int, AlignmentSymbol>>, std::array<unsigned int, 2>> mutform2count4map_fq;
    const PhredMutationTable sscs_mut_table(
                paramset.phred_max_sscs_transition_CG_TA, 
                paramset.phred_max_sscs_transition_TA_CG, 
                paramset.phred_max_sscs_transversion_any,
                paramset.phred_max_sscs_indel_open,
                paramset.phred_max_sscs_indel_ext);
    symbolToCountCoverageSet12.updateByRegion3Aln(
            mutform2count4map_bq, 
            mutform2count4map_fq,
            umi_strand_readset, 
            refstring,
            region_repeatvec,
            paramset.bq_phred_added_misma, 
            paramset.bq_phred_added_indel, 
            paramset.should_add_note, 
            paramset.phred_max_frag_indel_ext, 
            paramset.phred_max_frag_indel_basemax, 
            sscs_mut_table,
            minABQ_snv,
            ((ASSAY_TYPE_AMPLICON == inferred_assay_type) ? paramset.ess_georatio_dedup_amp : paramset.ess_georatio_dedup_cap),
            paramset.ess_georatio_duped_pcr,
            !paramset.disable_dup_read_merge, 
            is_loginfo_enabled, 
            thread_id, 
            paramset.fixedthresBQ, 
            paramset.nogap_phred,
            paramset.highqual_thres_snv, 
            paramset.highqual_thres_indel, 
            paramset.uni_bias_r_max,
            SEQUENCING_PLATFORM_IONTORRENT == paramset.sequencing_platform,
            inferred_assay_type,
            paramset.phred_frac_indel_error_before_barcode_labeling,
            paramset.baq_per_aligned_base,
            paramset.regside_nbases,
            ((ASSAY_TYPE_AMPLICON == inferred_assay_type) ? paramset.bias_flag_amp_snv : paramset.bias_flag_cap_snv),
            ((ASSAY_TYPE_AMPLICON == inferred_assay_type) ? paramset.bias_flag_amp_indel : paramset.bias_flag_cap_indel),
            0);
    if (is_loginfo_enabled) { LOG(logINFO) << "Thread " << thread_id << " starts analyzing phasing info"; }
    auto mutform2count4vec_bq = map2vector(mutform2count4map_bq);
    auto simplemut2indices_bq = mutform2count4vec_to_simplemut2indices(mutform2count4vec_bq);
    auto mutform2count4vec_fq = map2vector(mutform2count4map_fq);
    auto simplemut2indices_fq = mutform2count4vec_to_simplemut2indices(mutform2count4vec_fq);
    
    if (is_loginfo_enabled) { LOG(logINFO) << "Thread " << thread_id  << " starts generating block gzipped vcf"; }
    
    std::string buf_out_string;
    std::string buf_out_string_pass;
    
    const std::set<size_t> empty_size_t_set;
    const unsigned int capDP = 10*1000*1000;
    
    unsigned int gbDPmin = capDP;
    unsigned int gbDPmax = 0;
    unsigned int gcDPmin = capDP;
    unsigned int gcDPmax = 0;
    std::string gfGTmm2 = "./.";
    unsigned int gfGQmin = capDP;
    unsigned int gfGQmax = 0;
    
    unsigned int prevPosition = rpos_inclu_beg;
    SymbolType prevSymbolType = NUM_SYMBOL_TYPES;
    
    std::array<CoveredRegion<uint32_t>, 2> bq_ins_2bdepths = {{
        CoveredRegion<uint32_t>(tid, extended_inclu_beg_pos, extended_exclu_end_pos + 1), 
        CoveredRegion<uint32_t>(tid, extended_inclu_beg_pos, extended_exclu_end_pos + 1)
    }};
    std::array<CoveredRegion<uint32_t>, 2> bq_del_2bdepths = {{
        CoveredRegion<uint32_t>(tid, extended_inclu_beg_pos, extended_exclu_end_pos + 1),
        CoveredRegion<uint32_t>(tid, extended_inclu_beg_pos, extended_exclu_end_pos + 1)
    }};
    for (unsigned int refpos = extended_inclu_beg_pos; refpos <= extended_exclu_end_pos; refpos++) {
        for (unsigned int strand = 0; strand < 2; strand++) {
            unsigned int link_ins_cnt = 0;
            for (const auto ins_symbol : INS_SYMBOLS) {
                link_ins_cnt += symbolToCountCoverageSet12.bq_tsum_depth.at(strand).getByPos(refpos).getSymbolCount(ins_symbol);
            }
            if (extended_inclu_beg_pos < refpos) {
                bq_ins_2bdepths[strand].getRefByPos(refpos-1) += link_ins_cnt;
            }
            bq_ins_2bdepths[strand].getRefByPos(refpos) += link_ins_cnt;
 
            for (const auto del_symbol : DEL_SYMBOLS) {
                const auto & pos_to_dlen_to_data = symbolToCountCoverageSet12.bq_tsum_depth.at(strand).getPosToDlenToData(del_symbol);
                if (pos_to_dlen_to_data.find(refpos) != pos_to_dlen_to_data.end()) {
                    for (const auto & indel2data : pos_to_dlen_to_data.at(refpos)) {
                        unsigned int dlen = indel2data.first;
                        unsigned int cnt = indel2data.second;
                        if (extended_inclu_beg_pos < refpos) {
                            bq_del_2bdepths[strand].getRefByPos(refpos-1) += cnt;
                        }
                        for (unsigned d = 0; d <= dlen; d++) {
                            if ((refpos + d) <= rpos_exclu_end) {
                                bq_del_2bdepths[strand].getRefByPos(refpos+d) += cnt;
                            }
                        }
                    }
                }
            }
        }
    }
    // This code can result in false negative InDels, but it looks like these false negative InDels can be avoided with proper (but potentially very complex) normalization
    // Therefore, this code and all other code related to this code are commented out for now.
    /*
    std::array<CoveredRegion<uint32_t>, 2> bq_indel_adjmax_depths = {
        CoveredRegion<uint32_t>(tid, extended_inclu_beg_pos, extended_exclu_end_pos + 1),
        CoveredRegion<uint32_t>(tid, extended_inclu_beg_pos, extended_exclu_end_pos + 1)
    };
    for (unsigned int refpos = extended_inclu_beg_pos; refpos <= extended_exclu_end_pos; refpos++) {
        auto totins = bq_ins_2bdepths[0].getByPos(refpos) + bq_ins_2bdepths[1].getByPos(refpos);
        if (totins > 0) {
            for (unsigned int p2 = MAX(extended_inclu_beg_pos + 20, refpos) - 20; p2 <= MIN(refpos + 20, extended_exclu_end_pos); p2++) {
                bq_indel_adjmax_depths[0].getRefByPos(p2) = MAX(bq_indel_adjmax_depths[0].getByPos(p2), totins);
            }
        }
        auto totdel = bq_del_2bdepths[0].getByPos(refpos) + bq_del_2bdepths[1].getByPos(refpos);
        if (totdel > 0) {
            for (unsigned int p2 = MAX(extended_inclu_beg_pos + 20, refpos) - 20; p2 <= MIN(refpos + 20, extended_exclu_end_pos); p2++) {
                bq_indel_adjmax_depths[1].getRefByPos(p2) = MAX(bq_indel_adjmax_depths[1].getByPos(p2), totdel);
            }
        }
    }
    */
    RegionalTandemRepeat defaultRTR;
    
    for (unsigned int refpos = rpos_inclu_beg; refpos <= rpos_exclu_end; refpos++) {
        std::string repeatunit;
        unsigned int repeatnum = 0;
        
        unsigned int rridx = refpos - extended_inclu_beg_pos;
        indelpos_to_context(repeatunit, repeatnum, refstring, rridx);
        const RegionalTandemRepeat & rtr1 = ((rridx > 0)                           ? (region_repeatvec[rridx-1]) : defaultRTR);
        const RegionalTandemRepeat & rtr2 = ((rridx + 1 < region_repeatvec.size()) ? (region_repeatvec[rridx+1]) : defaultRTR);
        
        const std::array<SymbolType, 2> stype_to_immediate_prev = {{LINK_SYMBOL, BASE_SYMBOL}};
        for (SymbolType symbolType : SYMBOL_TYPES_IN_VCF_ORDER) {
            TumorKeyInfo THE_DUMMY_TUMOR_KEY_INFO;
            bcfrec::BcfFormat init_fmt;
            const AlignmentSymbol refsymbol = (LINK_SYMBOL == symbolType ? LINK_M : (
                    refstring.size() == (refpos - extended_inclu_beg_pos) ? BASE_NN : CHAR_TO_SYMBOL.data.at(refstring.at(refpos - extended_inclu_beg_pos))));
            std::array<unsigned int, 2> bDPcDP = BcfFormat_init(init_fmt, symbolToCountCoverageSet12, refpos, symbolType, !paramset.disable_dup_read_merge, refsymbol);
            AlignmentSymbol most_confident_symbol = END_ALIGNMENT_SYMBOLS;
            float most_confident_qual = 0;
            std::string most_confident_GT = "./.";
            float most_confident_GQ = 0;
            
            AlignmentSymbol most_confident_nonref_symbol = (LINK_SYMBOL == symbolType ? LINK_NN : BASE_NN);
            float most_confident_nonref_qual = -FLT_MAX;
            AlignmentSymbol most_confident_nonref_symbol_2 = most_confident_nonref_symbol;
            float most_confident_nonref_qual_2 = most_confident_nonref_qual;
            float refqual = 0;
            //std::vector<bcfrec::BcfFormat> fmts(SYMBOL_TYPE_TO_INCLU_END[symbolType] - SYMBOL_TYPE_TO_INCLU_BEG[symbolType] + 1, init_fmt);
            // ., , indelstring, bdepth, tkiidx
            std::vector<std::tuple<bcfrec::BcfFormat, AlignmentSymbol, unsigned int, std::string, unsigned int>> fmts;
            if (rpos_exclu_end != refpos && bDPcDP[0] >= paramset.min_depth_thres) {
                std::vector<AlignmentSymbol> majorsymbols;
                for (AlignmentSymbol symbol = SYMBOL_TYPE_TO_INCLU_BEG[symbolType]; symbol <= SYMBOL_TYPE_TO_INCLU_END[symbolType]; symbol = AlignmentSymbol(1+(unsigned int)symbol)) {
                    auto symbol_bdepth = 
                          symbolToCountCoverageSet12.bq_tsum_depth.at(0).getByPos(refpos).getSymbolCount(symbol)
                        + symbolToCountCoverageSet12.bq_tsum_depth.at(1).getByPos(refpos).getSymbolCount(symbol);
                    if (symbol_bdepth * 4 > bDPcDP[0]);
                    majorsymbols.push_back(symbol);
                }
                if (majorsymbols.size() == 0) {
                    majorsymbols.push_back(refsymbol);
                }
                for (AlignmentSymbol symbol = SYMBOL_TYPE_TO_INCLU_BEG[symbolType]; symbol <= SYMBOL_TYPE_TO_INCLU_END[symbolType]; symbol = AlignmentSymbol(1+(unsigned int)symbol)) {
                    unsigned int phred_max_sscs_all = 200;
                    for (auto majorsymbol : majorsymbols) {
                        UPDATE_MIN(phred_max_sscs_all, sscs_mut_table.toPhredErrRate(majorsymbol, symbol));
                    }
                    const bool is_rescued = (
                            extended_posidx_to_is_rescued[refpos - extended_inclu_beg_pos] &&
                            (tid_pos_symb_to_tkis.end() != tid_pos_symb_to_tkis.find(std::make_tuple(tid, refpos, symbol)))); 
                    unsigned int phred_max_sscs = sscs_mut_table.toPhredErrRate(refsymbol, symbol);
                    const auto bdepth = symbolToCountCoverageSet12.bq_tsum_depth.at(0).getByPos(refpos).getSymbolCount(symbol)
                                      + symbolToCountCoverageSet12.bq_tsum_depth.at(1).getByPos(refpos).getSymbolCount(symbol);
                    const bool pass_thres = (bdepth >= paramset.min_altdp_thres);
                    if ((!is_rescued) && (!pass_thres) && (refsymbol != symbol)) {
                        continue;
                    }
                    
                    const auto simplemut = std::make_pair(refpos, symbol);
                    auto indices_bq = (simplemut2indices_bq.find(simplemut) != simplemut2indices_bq.end() ? simplemut2indices_bq[simplemut] : empty_size_t_set); 
                    auto indices_fq = (simplemut2indices_fq.find(simplemut) != simplemut2indices_fq.end() ? simplemut2indices_fq[simplemut] : empty_size_t_set);
                    std::vector<TumorKeyInfo> tkis;
                    if (is_rescued) {
                        tkis = tid_pos_symb_to_tkis.find(std::make_tuple(tid, refpos, symbol))->second;
                    }
                    
                    auto fmt = init_fmt;
                    int tkiidx = 0;
                    std::vector<std::tuple<unsigned int, std::string, int>> bad0a_indelstring_tkiidx_vec;
                    if (isSymbolIns(symbol) || isSymbolDel(symbol)) {
                        for (size_t strand = 0; strand < 2; strand++) {
                            fill_by_indel_info(
                                    fmt, 
                                    symbolToCountCoverageSet12,
                                    strand,
                                    refpos,
                                    symbol,
                                    refstring,
                                    repeatunit,
                                    repeatnum
                            );
                        }
                        if (is_rescued) {
                            for (const auto & tki : tkis) {
                                unsigned int tabpos = tki.ref_alt.find("\t");
                                const auto & vcfref = tki.ref_alt.substr(0, tabpos);
                                const auto & vcfalt = tki.ref_alt.substr(tabpos + 1);
                                std::string indelstring;
                                if (vcfref.size() > vcfalt.size()) {
                                    indelstring = vcfref.substr(vcfalt.size());
                                } else {
                                    assert (vcfref.size() < vcfalt.size());
                                    indelstring = vcfalt.substr(vcfref.size());
                                }
                                bad0a_indelstring_tkiidx_vec.push_back(std::make_tuple(bdepth, indelstring, tkiidx));
                                tkiidx++;
                            }
                        } else {
                            // bad0a_indelstring_tkiidx_vec.push_back(std::make_tuple(bdepth, std::string(""), (is_rescued ? 0 : -1)));
                            const auto bad0a_indelstring_pairvec = indel_get_majority(
                                    fmt, /*prev_is_tumor, tki, false, */
                                    std::get<0>(tname_tseqlen_tuple).c_str(), 
                                    refpos, 
                                    symbol, 
                                    true);
                            for (const auto & bad0a_indelstring_pair : bad0a_indelstring_pairvec) {
                                bad0a_indelstring_tkiidx_vec.push_back(std::make_tuple(bad0a_indelstring_pair.first, bad0a_indelstring_pair.second, -1));
                            }
                        }
                    } else {
                        bad0a_indelstring_tkiidx_vec.push_back(std::make_tuple(bdepth, std::string(""), (is_rescued ? 0 : -1)));
                    }
                    
                    for (const auto & bad0a_indelstring_tkiidx : bad0a_indelstring_tkiidx_vec) {
                        fill_by_symbol(fmt, //[symbol - SYMBOL_TYPE_TO_INCLU_BEG[symbolType]], 
                            symbolToCountCoverageSet12, 
                            refpos, 
                            symbol, 
                            refstring,
                            extended_inclu_beg_pos, 
                            mutform2count4vec_bq, 
                            indices_bq, 
                            mutform2count4vec_fq, 
                            indices_fq,
                            ((BASE_SYMBOL == symbolType) ? minABQ_snv : minABQ_indel),
                            paramset.minMQ1,
                            paramset.maxMQ,
                            phred_max_sscs,
                            paramset.phred_max_dscs_all,
                            !paramset.disable_dup_read_merge,
                            !paramset.enable_dup_read_vqual,
                            is_rescued,
                            (NOT_PROVIDED != paramset.vcf_tumor_fname),
                            repeatunit,
                            repeatnum,
                            (is_rescued ? tkis.at(std::get<2>(bad0a_indelstring_tkiidx)) : THE_DUMMY_TUMOR_KEY_INFO),
                            paramset.any_mul_contam_frac,
                            paramset.t2n_mul_contam_frac,
                            paramset.t2n_add_contam_frac,
                            paramset.t2n_add_contam_transfrac,
                            paramset.min_edge_dist,
                            paramset.central_readlen,
                            paramset.baq_per_aligned_base,
                            paramset.powlaw_exponent,
                            bq_ins_2bdepths,
                            bq_del_2bdepths,
                            paramset.somaticGT,
                            (paramset.ref_bias_awareness & ((ASSAY_TYPE_AMPLICON == inferred_assay_type) ? 0x1 : 0x2)),
                            // ((ASSAY_TYPE_AMPLICON == inferred_assay_type) ? paramset.phred_umi_dimret_qual : ((double)FLT_MAX)),
                            (is_umi_barcoded ? ((BASE_SYMBOL == symbolType) ? paramset.phred_umi_dimret_mult_snv : paramset.phred_umi_dimret_mult_indel) : 1.0),
                            // bq_indel_adjmax_depths,
                            (SEQUENCING_PLATFORM_IONTORRENT == paramset.sequencing_platform ? 200.0 : ((ASSAY_TYPE_AMPLICON == inferred_assay_type) ? paramset.amp_BQ_sqr_coef : paramset.cap_BQ_sqr_coef)),
                            paramset.phred_varcall_err_per_map_err_per_base,
                            paramset.phred_snv_to_indel_ratio,
                            (SEQUENCING_PLATFORM_IONTORRENT == paramset.sequencing_platform), 
                            
                            paramset.powlaw_anyvar_base,
                            (init_fmt.bDP > init_fmt.DP * paramset.highqual_min_ratio), // tUseHD1
                            paramset.phred_triallelic_indel,
                            (paramset.phred_max_dscs_all - paramset.phred_pow_dscs_origin),
                            (phred_max_sscs_all  - ((BASE_SYMBOL == symbolType) ? paramset.phred_pow_sscs_origin : paramset.phred_pow_sscs_indel_origin)),
                            std::get<0>(bad0a_indelstring_tkiidx),
                            std::get<1>(bad0a_indelstring_tkiidx),
                            0);
                        fmts.push_back(std::make_tuple(fmt, symbol, std::get<0>(bad0a_indelstring_tkiidx), std::get<1>(bad0a_indelstring_tkiidx), std::get<2>(bad0a_indelstring_tkiidx)));
                    }
                }
                // for (AlignmentSymbol symbol = SYMBOL_TYPE_TO_INCLU_BEG[symbolType]; symbol <= SYMBOL_TYPE_TO_INCLU_END[symbolType]; symbol = AlignmentSymbol(1+(unsigned int)symbol)) {
                for (const auto & fmtinfo : fmts) {
                    const auto &fmt = std::get<0>(fmtinfo);
                    const auto symbol = std::get<1>(fmtinfo);
                    const auto & vaqvec = fmt.VAQ;
                    auto vaq = (vaqvec.size() == 0 ? 0 : vaqvec[0]);
                    if (refsymbol == symbol) { refqual = vaq; }
                    if (vaq >= most_confident_qual) {
                        most_confident_symbol = symbol;
                        most_confident_qual = vaq;
                        most_confident_GT = fmt.GT;
                        most_confident_GQ = fmt.GQ;
                    }
                    if (vaq >= most_confident_nonref_qual && refsymbol != symbol) {
                        most_confident_nonref_symbol_2 = most_confident_nonref_symbol;
                        most_confident_nonref_qual_2 = most_confident_nonref_qual;
                        most_confident_nonref_symbol = symbol;
                        most_confident_nonref_qual = vaq;
                    }
                }
            }
            if ((rpos_exclu_end != refpos || LINK_SYMBOL == symbolType) && (paramset.outvar_flag & OUTVAR_NONREF)) {
                auto bDPval = bDPcDP[0];
                auto cDPval = bDPcDP[1];
                auto fGTmm2 = (most_confident_GQ >= 10 ? most_confident_GT : "./.");
                auto fGQval = most_confident_GQ;
                bool gbDPhasWideRange = (is_sig_out(bDPval, gbDPmin, gbDPmax, 130,  3) || (0 == gbDPmax && bDPval > gbDPmax) || (0 == bDPval && gbDPmin > bDPval));
                bool gcDPhasWideRange = (is_sig_out(cDPval, gcDPmin, gcDPmax, 130,  3));
                bool gfGQhasWideRange = (is_sig_out(fGQval, gfGQmin, gfGQmax, 130, 10) || (std::string(fGTmm2) != gfGTmm2));
                if (gbDPhasWideRange || gcDPhasWideRange || gfGQhasWideRange ||
                        (refpos - prevPosition >= G_BLOCK_SIZE) || (refpos == rpos_exclu_end)) {
                    auto iendPosition = (LINK_SYMBOL == symbolType ? (refpos - 1) : refpos);
                    auto iendSymbolType = stype_to_immediate_prev[symbolType];
                    
                    unsigned int begpos = (BASE_SYMBOL == prevSymbolType ? (prevPosition+1) : prevPosition);
                    unsigned int endpos = (BASE_SYMBOL == iendSymbolType ? (iendPosition+1) : iendPosition);
                    
                    std::string genomicInfoString = ((0 == gbDPmax || (bcf_hdr != NULL && paramset.is_tumor_format_retrieved)) ? "" : genomic_reg_info_to_string(
                            std::get<0>(tname_tseqlen_tuple),
                            begpos, prevSymbolType,
                            endpos, iendSymbolType,
                            gbDPmin, gcDPmin,
                            gfGTmm2, gfGQmin,
                            refstring, extended_inclu_beg_pos));
                    raw_out_string += genomicInfoString + buf_out_string;
                    raw_out_string_pass += genomicInfoString + buf_out_string_pass;
                    buf_out_string.clear();
                    buf_out_string_pass.clear();
                    prevPosition = refpos;
                    prevSymbolType = symbolType;
                    gbDPmin = bDPval;
                    gbDPmax = bDPval;
                    gcDPmin = cDPval;
                    gcDPmax = cDPval;
                    gfGTmm2 = fGTmm2;
                    gfGQmin = fGQval;
                    gfGQmax = fGQval;
                } else {
                    UPDATE_MIN(gbDPmin, bDPval);
                    UPDATE_MAX(gbDPmax, bDPval);
                    UPDATE_MIN(gcDPmin, cDPval);
                    UPDATE_MAX(gcDPmax, cDPval);
                    UPDATE_MIN(gfGQmin, fGQval);
                    UPDATE_MAX(gfGQmax, fGQval);
                }
            }
            if (rpos_exclu_end != refpos && bDPcDP[0] >= paramset.min_depth_thres) {
                bool is_probably_germline = false;
                //for (AlignmentSymbol symbol = SYMBOL_TYPE_TO_INCLU_BEG[symbolType]; symbol <= SYMBOL_TYPE_TO_INCLU_END[symbolType] + n_additional_symbols; symbol = AlignmentSymbol(1+(unsigned int)symbol)) {
                for (auto & fmtinfo : fmts) {
                    auto & fmt = std::get<0>(fmtinfo);
                    auto & symbol = std::get<1>(fmtinfo);
                    auto & tkiidx = std::get<4>(fmtinfo);
                    // auto & fmt = fmts[symbol - SYMBOL_TYPE_TO_INCLU_BEG[symbolType]];
                    const bool pass_thres = ((fmt.bAD1[0] + fmt.bAD1[1]) >= paramset.min_altdp_thres);
                    const bool is_rescued = (tkiidx >= 0); //(
                            //extended_posidx_to_is_rescued[refpos - extended_inclu_beg_pos] &&
                            //(tid_pos_symb_to_tkis.end() != tid_pos_symb_to_tkis.find(std::make_tuple(tid, refpos, symbol)))); 
                    std::vector<TumorKeyInfo> tkis;
                    if (is_rescued) {
                        tkis = tid_pos_symb_to_tkis.find(std::make_tuple(tid, refpos, symbol))->second; 
                    }
                    if (pass_thres && symbol != refsymbol) {
                        is_probably_germline = true;
                    }
                    const bool will_generate_out = (is_rescued ? (paramset.outvar_flag & OUTVAR_SOMATIC) : (pass_thres && (paramset.outvar_flag & OUTVAR_ANY)));
                    if (will_generate_out || (refsymbol == symbol)) {
                        auto nonref_symbol_12 = (most_confident_nonref_symbol != symbol ? most_confident_nonref_symbol : most_confident_nonref_symbol_2);
                        auto nonref_qual_12 = (most_confident_nonref_symbol != symbol ? most_confident_nonref_qual : most_confident_nonref_qual_2);
                        fmt.OType = SYMBOL_TO_DESC_ARR[nonref_symbol_12];
                        // const auto & vaqvec = fmts[refsymbol - SYMBOL_TYPE_TO_INCLU_BEG[symbolType]].VAQ;
                        // auto ref_qual = (vaqvec.size() == 0 ? 0 : vaqvec[0]);
                        fmt.ORAQs = {{ ((float)nonref_qual_12), ((float)refqual) }};
                        unsigned int phred_max_sscs = sscs_mut_table.toPhredErrRate(refsymbol, symbol);
                        append_vcf_record(buf_out_string, 
                                buf_out_string_pass, 
                                arg.vc_stats,
                                symbolToCountCoverageSet12,
                                std::get<0>(tname_tseqlen_tuple).c_str(),
                                refpos, 
                                symbol, 
                                fmt,
                                refstring, 
                                extended_inclu_beg_pos, 
                                paramset.vqual, 
                                should_output_all, 
                                should_let_all_pass,
                                (is_rescued ? tkis.at(tkiidx) : THE_DUMMY_TUMOR_KEY_INFO),
                                (NOT_PROVIDED != paramset.vcf_tumor_fname),
                                paramset.phred_germline_polymorphism,
                                paramset.uni_bias_thres, // = 180
                                bcf_hdr, 
                                paramset.is_tumor_format_retrieved,
                                ((BASE_SYMBOL == symbolType) ? paramset.highqual_thres_snv : (LINK_SYMBOL == symbolType ? paramset.highqual_thres_indel : 0)),
                                paramset.highqual_min_ratio,
                                paramset.any_mul_contam_frac,
                                paramset.t2n_mul_contam_frac,
                                paramset.t2n_add_contam_frac,
                                paramset.t2n_add_contam_transfrac,
                                repeatunit, 
                                repeatnum,
                                // SEQUENCING_PLATFORM_IONTORRENT == paramset.sequencing_platform,
                                // paramset.maxMQ,
                                paramset.central_readlen,
                                paramset.phred_triallelic_indel,
                                phred_max_sscs,
                                paramset.phred_max_dscs_all,
                                ((BASE_SYMBOL == symbolType) ? paramset.phred_pow_sscs_origin : paramset.phred_pow_sscs_indel_origin),
                                paramset.phred_pow_dscs_origin,
                                paramset.vad,
                                paramset.is_somatic_snv_filtered_by_any_nonref_germline_snv,
                                paramset.is_somatic_indel_filtered_by_any_nonref_germline_indel,
                                // ((ASSAY_TYPE_AMPLICON == inferred_assay_type) ? paramset.amp_BQ_sqr_coef : paramset.cap_BQ_sqr_coef),
                                // paramset.phred_varcall_err_per_map_err_per_base,
                                paramset.powlaw_exponent,
                                paramset.powlaw_anyvar_base,
                                paramset.syserr_maxqual,
                                paramset.syserr_norm_devqual,
                                // paramset.phred_umi_dimret_qual,
                                paramset.phred_umi_dimret_mult_indel,
                                paramset.bitflag_InDel_penal_t_UMI_n_UMI,
                                paramset.haplo_in_diplo_allele_perc,
                                paramset.diplo_oneside_posbias_perc,
                                paramset.diplo_twoside_posbias_perc,
                                paramset.haplo_oneside_posbias_perc,
                                paramset.haplo_twoside_posbias_perc,
                                paramset.phred_snv_to_indel_ratio,
                                rtr1,
                                rtr2,
                                // std::get<2>(fmtinfo), // indel bdepth
                                std::get<3>(fmtinfo), // indel string
                                0);
                    }
                }
                
                if (is_probably_germline && paramset.outvar_flag & OUTVAR_GERMLINE) {
                    int indelph = 40;
                    if (rtr1.tracklen >= 8 && rtr1.unitlen >= 1) {
                        indelph = MIN(indelph, 40 - (int)indel_phred(8.0*8.0, rtr1.unitlen, rtr1.unitlen, rtr1.tracklen / rtr1.unitlen));
                    }
                    if (rtr2.tracklen >= 8 && rtr2.unitlen >= 1) {
                        indelph = MIN(indelph, 40 - (int)indel_phred(8.0*8.0, rtr2.unitlen, rtr2.unitlen, rtr2.tracklen / rtr2.unitlen));
                    }
                    indelph = MAX(6, indelph);
                    std::vector<std::pair<AlignmentSymbol, bcfrec::BcfFormat*>> symbol_format_vec;
                    //for (AlignmentSymbol symbol = SYMBOL_TYPE_TO_INCLU_BEG[symbolType]; 
                    //     symbol <= SYMBOL_TYPE_TO_INCLU_END[symbolType]; 
                    //     symbol = AlignmentSymbol(1+(unsigned int)symbol)) {
                    for (auto & fmtinfo : fmts) {
                        auto & fmt = std::get<0>(fmtinfo);
                        auto & symbol = std::get<1>(fmtinfo);
                        // auto & fmt = fmts[symbol - SYMBOL_TYPE_TO_INCLU_BEG[symbolType]];
                        if (refsymbol == symbol) {
                            auto central_readlen = MAX(paramset.central_readlen, 30U); 
                            double ref_bias = (double)MIN(fmt.RefBias, central_readlen - 30U) / (double)central_readlen;
                            // double con_bias = MIN(fmt.DP * 2e-4, paramset.any_mul_contam_frac);
                            double aln_bias = mathsquare((double)SUMVEC(fmt.aNMAD) / ((double)SUMVEC(fmt.aAD) + DBL_EPSILON) / (NM_MULT_NORM_COEF)) / 2.0; // / 10.0;
                            double biasfrac_binom = MIN(0.9, MAX3(0.004, ref_bias, aln_bias) + pow(0.1, indelph/10.0));
                            double biasfrac_power = MIN(0.9, MAX3(0.004, ref_bias, paramset.any_mul_contam_frac) + pow(0.1, indelph/10.0));
                            
                            double    ref_dep = compute_norm_ad(&fmt, BASE_SYMBOL == symbolType);
                            double nonref_dep = SUM2(fmt.cDPTT) - ref_dep;
                            
                            fmt.BLODQ = MAX(0, (int)MIN(
                                    calc_binom_10log10_likeratio(biasfrac_binom, ref_dep, ref_dep + MAX(DBL_EPSILON, nonref_dep)),
                                    paramset.powlaw_exponent * (10.0/log(10.0)) * log((ref_dep + 0.5) / (nonref_dep + 0.5) / biasfrac_power)));
                            fmt.note += other_join(std::array<double, 4>{{ ref_bias, aln_bias, biasfrac_binom, biasfrac_power }}, "#") + "##";
                        } else {
                            fmt.BLODQ = 999;
                        }
                        symbol_format_vec.push_back(std::make_pair(symbol, &fmt));
                    }
                    while (symbol_format_vec.size() < 4) {
                        symbol_format_vec.push_back(std::make_pair(END_ALIGNMENT_SYMBOLS, &init_fmt));
                    }
                    // TumorKeyInfo tki;
                    output_germline(
                            buf_out_string_pass,
                            refsymbol,
                            symbol_format_vec,
                            std::get<0>(tname_tseqlen_tuple).c_str(), // tname,
                            refstring,
                            refpos,
                            extended_inclu_beg_pos, // regionpos,
                            paramset.central_readlen,
                            // tki, 
                            0);
                }
            }
        }    
    }
    if (is_loginfo_enabled) { LOG(logINFO) << "Thread " << thread_id  << " starts destroying bam records"; }
    for (auto strand_readset : umi_strand_readset) {
        for (unsigned int strand = 0; strand < 2; strand++) {
            auto readset = strand_readset.first[strand]; 
            for (auto read : readset) {
                for (bam1_t *b : read) {
                    bam_destroy1(b);
                } 
            }
        }
    }
    bgzip_string(outstring_allp, raw_out_string);
    if (!is_vcf_out_pass_to_stdout) {
        bgzip_string(outstring_pass, raw_out_string_pass);
    } else {
        outstring_pass += raw_out_string_pass;
    }
    if (is_loginfo_enabled) { LOG(logINFO) << "Thread " << thread_id  << " is done with current task"; }
    return 0;
};

int 
main(int argc, char **argv) {
    std::clock_t c_start = std::clock();
    auto t_start = std::chrono::high_resolution_clock::now();
    
    const char *UMI_STRUCT = getenv("ONE_STEP_UMI_STRUCT");
    const std::string UMI_STRUCT_STRING = ((UMI_STRUCT != NULL && strlen(UMI_STRUCT) > 0) ? std::string(UMI_STRUCT) : std::string(""));
    CommandLineArgs paramset;
    int parsing_result_flag = -1;
    SequencingPlatform inferred_sequencing_platform = SEQUENCING_PLATFORM_AUTO;
    int parsing_result_ret = paramset.initFromArgCV(parsing_result_flag, inferred_sequencing_platform, argc, argv);
    if (parsing_result_ret || parsing_result_flag) {
        return parsing_result_ret; 
    }
    LOG(logINFO) << "Program " << argv[0] << " version " << VERSION_DETAIL;
    LOG(logINFO) << "<GIT_DIFF_FULL_DISPLAY_MSG>"; 
    LOG(logINFO) << GIT_DIFF_FULL;
    LOG(logINFO) << "</GIT_DIFF_FULL_DISPLAY_MSG>";
    
    std::vector<std::tuple<std::string, unsigned int>> tid_to_tname_tseqlen_tuple_vec;
    samfname_to_tid_to_tname_tseq_tup_vec(tid_to_tname_tseqlen_tuple_vec, paramset.bam_input_fname);
    
    const unsigned int nthreads = paramset.max_cpu_num;
    bool is_vcf_out_empty_string = (std::string("") == paramset.vcf_output_fname);
    BGZF *fp_allp = NULL;
    if (!is_vcf_out_empty_string) { 
        fp_allp = bgzf_open(paramset.vcf_output_fname.c_str(), "w");
        if (NULL == fp_allp) {
            LOG(logERROR) << "Unable to open the bgzip file " << paramset.vcf_output_fname;
            exit(-8);
        }
    }
    bool is_vcf_out_pass_empty_string = (std::string("") == paramset.vcf_out_pass_fname);
    bool is_vcf_out_pass_to_stdout = (std::string("-") == paramset.vcf_out_pass_fname);
    BGZF *fp_pass = NULL;
    if (!is_vcf_out_pass_empty_string && !is_vcf_out_pass_to_stdout) {  
        fp_pass = bgzf_open(paramset.vcf_out_pass_fname.c_str(), "w");
        if (NULL == fp_pass) {
            LOG(logERROR) << "Unable to open the bgzip file " << paramset.vcf_out_pass_fname;
            exit(-9);
        }
    }
    // Commented out for now due to lack of good documentation for these bgzf APIs. Can investigate later.
    /*
    if (paramset.vcf_output_fname.size() != 0 && paramset.vcf_output_fname != "-") {
        bgzf_index_build_init(fp_allp);
    }
    */
    // bgzf_mt(fp_allp, nthreads, 128);
    // samFile *sam_infile = sam_open(paramset.bam_input_fname.c_str(), "r");
    
    std::ofstream bed_out;
    if (NOT_PROVIDED != paramset.bed_out_fname) {
        bed_out.open(paramset.bed_out_fname, std::ios::out);
    }

#if defined(USE_STDLIB_THREAD)
    const unsigned int nidxs = nthreads * 2 + 1;
#else
    const unsigned int nidxs = nthreads;
#endif
    
    bcf_hdr_t *g_bcf_hdr = NULL;
    const char *g_sample = NULL;
    if (NOT_PROVIDED != paramset.vcf_tumor_fname) {
        htsFile *infile = hts_open(paramset.vcf_tumor_fname.c_str(), "r");
        g_bcf_hdr = bcf_hdr_read(infile);
        g_sample = "";
        if (bcf_hdr_nsamples(g_bcf_hdr) > 0) {
            g_sample = g_bcf_hdr->samples[0];
        };
        bcf_close(infile);
    }
    std::vector<hts_idx_t*> sam_idxs(nidxs, NULL);
    std::vector<samFile*> samfiles(nidxs, NULL);
    std::vector<faidx_t*> ref_faidxs(nidxs, NULL);
    std::vector<bcf_srs_t*> srs(nidxs, NULL);
    for (size_t i = 0; i < nidxs; i++) {
        samfiles[i] = sam_open(paramset.bam_input_fname.c_str(), "r");
        if (NULL == samfiles[i]) {
            LOG(logCRITICAL) << "Failed to load BAM file " << paramset.bam_input_fname << " for thread with ID = " << i;
            exit(-3);
        }
        sam_idxs[i] = sam_index_load(samfiles[i], paramset.bam_input_fname.c_str());
        if (NULL == sam_idxs[i]) {
            LOG(logCRITICAL) << "Failed to load BAM index " << paramset.bam_input_fname << " for thread with ID = " << i;
            exit(-4);
        }
        if (paramset.fasta_ref_fname.size() > 0) {
            ref_faidxs[i] = fai_load(paramset.fasta_ref_fname.c_str());
            if (NULL == ref_faidxs[i]) {
                LOG(logCRITICAL) << "Failed to load reference index for file " << paramset.fasta_ref_fname << " for thread with ID = " << i;
                exit(-5);
            }
        }
    }
    
    VcStats all_vc_stats;

    bam_hdr_t * samheader = sam_hdr_read(samfiles[0]);
    std::string header_outstring = generate_vcf_header(paramset.fasta_ref_fname.c_str(), 
            SEQUENCING_PLATFORM_TO_DESC.at(inferred_sequencing_platform).c_str(), 
            paramset.central_readlen, 
            paramset.minABQ_pcr_snv, 
            paramset.minABQ_pcr_indel, 
            paramset.minABQ_cap_snv, 
            paramset.minABQ_cap_indel, 
            argc, 
            argv, 
            samheader->n_targets, 
            samheader->target_name, 
            samheader->target_len,
            paramset.sample_name.c_str(), 
            g_sample, 
            paramset.is_tumor_format_retrieved);
    clearstring<false>(fp_allp, header_outstring);
    clearstring<false>(fp_pass, header_outstring, is_vcf_out_pass_to_stdout);

    std::vector<std::tuple<unsigned int, unsigned int, unsigned int, bool, unsigned int>> tid_beg_end_e2e_tuple_vec1;
    std::vector<std::tuple<unsigned int, unsigned int, unsigned int, bool, unsigned int>> tid_beg_end_e2e_tuple_vec2;
    std::map<std::tuple<unsigned int, unsigned int, AlignmentSymbol>, std::vector<TumorKeyInfo>> tid_pos_symb_to_tkis1; 
    std::map<std::tuple<unsigned int, unsigned int, AlignmentSymbol>, std::vector<TumorKeyInfo>> tid_pos_symb_to_tkis2; 
    SamIter samIter(paramset.bam_input_fname, paramset.tier1_target_region, paramset.bed_region_fname, nthreads); 
    unsigned int n_sam_iters = 0;    
    int iter_nreads = samIter.iternext(tid_beg_end_e2e_tuple_vec1);
    LOG(logINFO) << "PreProcessed " << iter_nreads << " reads in super-contig no " << (n_sam_iters);
    // rescue_variants_from_vcf
    tid_pos_symb_to_tkis1 = rescue_variants_from_vcf(tid_beg_end_e2e_tuple_vec1, tid_to_tname_tseqlen_tuple_vec, paramset.vcf_tumor_fname, g_bcf_hdr, paramset.is_tumor_format_retrieved);
    LOG(logINFO) << "Rescued/retrieved " << tid_pos_symb_to_tkis1.size() << " variants in super-contig no " << (n_sam_iters);
    while (iter_nreads > 0) {
        n_sam_iters++;
        std::thread read_bam_thread([&tid_beg_end_e2e_tuple_vec2, &tid_pos_symb_to_tkis2, &samIter, &iter_nreads, &n_sam_iters, &paramset, &tid_to_tname_tseqlen_tuple_vec, g_bcf_hdr]() {
            tid_beg_end_e2e_tuple_vec2.clear();
            iter_nreads = samIter.iternext(tid_beg_end_e2e_tuple_vec2);
            LOG(logINFO) << "PreProcessed " << iter_nreads << " reads in super-contig no " << (n_sam_iters);
            
            tid_pos_symb_to_tkis2 = rescue_variants_from_vcf(tid_beg_end_e2e_tuple_vec2, tid_to_tname_tseqlen_tuple_vec, paramset.vcf_tumor_fname, g_bcf_hdr, paramset.is_tumor_format_retrieved);
            LOG(logINFO) << "Rescued/retrieved " << tid_pos_symb_to_tkis2.size() << " variants in super-contig no " << (n_sam_iters);
        });
        const auto & tid_beg_end_e2e_tuple_vec = tid_beg_end_e2e_tuple_vec1; 
        const std::string bedstring_header = std::string("The BED-genomic-region is as follows (") + std::to_string(tid_beg_end_e2e_tuple_vec.size()) 
                + " chunks) for super-contig no " + std::to_string(n_sam_iters-1) + "\n";
        std::string bedstring = "";
        for (const auto & tid_beg_end_e2e_tuple : tid_beg_end_e2e_tuple_vec) {
            bedstring += (std::get<0>(tid_to_tname_tseqlen_tuple_vec[std::get<0>(tid_beg_end_e2e_tuple)]) + "\t"
                  + std::to_string(std::get<1>(tid_beg_end_e2e_tuple)) + "\t"
                  + std::to_string(std::get<2>(tid_beg_end_e2e_tuple)) + "\t"
                  + std::to_string(std::get<3>(tid_beg_end_e2e_tuple)) + "\t"
                  + "NumberOfReadsInThisInterval\t"
                  + std::to_string(std::get<4>(tid_beg_end_e2e_tuple)) + "\t" 
                  + "\n");
        }
        LOG(logINFO) << bedstring_header << bedstring;
        if (bed_out.is_open()) { bed_out << bedstring; }
        const unsigned int allridx = 0;  
        const unsigned int incvalue = tid_beg_end_e2e_tuple_vec.size();
        
        unsigned int nreads = 0;
        unsigned int npositions = 0;
        for (unsigned int j = 0; j < incvalue; j++) {
            auto region_idx = allridx + j;
            nreads += std::get<4>(tid_beg_end_e2e_tuple_vec[region_idx]);
            npositions += std::get<2>(tid_beg_end_e2e_tuple_vec[region_idx]) - std::get<1>(tid_beg_end_e2e_tuple_vec[region_idx]); 
        }
        
        assert(incvalue > 0);
        
        // distribute inputs as evenly as possible
#if defined(USE_STDLIB_THREAD)
        const unsigned int UNDERLOAD_RATIO = 1;
#else
        const unsigned int UNDERLOAD_RATIO = 4;
#endif
        unsigned int curr_nreads = 0;
        unsigned int curr_npositions = 0;
        unsigned int curr_zerobased_region_idx = 0;
        std::vector<std::pair<unsigned int, unsigned int>> beg_end_pair_vec;
        for (unsigned int j = 0; j < incvalue; j++) {
            auto region_idx = allridx + j;
            curr_nreads += std::get<4>(tid_beg_end_e2e_tuple_vec[region_idx]);
            curr_npositions += std::get<2>(tid_beg_end_e2e_tuple_vec[region_idx]) - std::get<1>(tid_beg_end_e2e_tuple_vec[region_idx]); 
            if (curr_nreads * nthreads * UNDERLOAD_RATIO > nreads || curr_npositions * nthreads * UNDERLOAD_RATIO > npositions || (j == incvalue - 1)) {
                beg_end_pair_vec.push_back(std::make_pair(curr_zerobased_region_idx, j+1));
                curr_nreads = 0;
                curr_npositions = 0;
                curr_zerobased_region_idx = j+1;
            }
        }
        
        LOG(logINFO) << "Will process the chunks from " << allridx << " to " << allridx + incvalue
                << " which contains approximately " << nreads << " reads and " << npositions << " positions divided into " 
                << beg_end_pair_vec.size() << " sub-chunks";
        
#if defined(USE_STDLIB_THREAD)
        if ( nidxs <= beg_end_pair_vec.size()) {abort();}
        std::vector<std::thread> threads; 
        threads.reserve(beg_end_pair_vec.size());
#endif
        std::vector<BatchArg> batchargs;
        batchargs.reserve(beg_end_pair_vec.size());
        for (unsigned int beg_end_pair_idx = 0; beg_end_pair_idx < beg_end_pair_vec.size(); beg_end_pair_idx++) {
            struct BatchArg a = {
                    outstring_allp : "",
                    outstring_pass : "",
                    vc_stats : VcStats(),
                    thread_id : 0,
                    hts_idx : NULL, 
                    ref_faidx : NULL,
                    bcf_hdr : g_bcf_hdr,
                    sr : NULL,
                    
                    tid_beg_end_e2e_tuple : tid_beg_end_e2e_tuple_vec.at(0),
                    tname_tseqlen_tuple : tid_to_tname_tseqlen_tuple_vec.at(0),
                    region_ordinal : n_sam_iters,
                    region_tot_num : (unsigned int)(INT32_MAX - 1),
                    regionbatch_ordinal : 0,
                    regionbatch_tot_num : 0,

                    paramset : paramset, 
                    UMI_STRUCT_STRING : UMI_STRUCT_STRING,
                    is_vcf_out_pass_to_stdout : is_vcf_out_pass_to_stdout,
                    is_vcf_out_empty_string : is_vcf_out_empty_string
            };
            batchargs.push_back(a);
        }
        unsigned int beg_end_pair_size = beg_end_pair_vec.size();

#if defined(_OPENMP) && !defined(USE_STDLIB_THREAD)
#pragma omp parallel for schedule(dynamic, 1) num_threads(nthreads)
#endif
        for (unsigned int beg_end_pair_idx = 0; beg_end_pair_idx < beg_end_pair_size; beg_end_pair_idx++) {
            
#if defined(_OPENMP) && !defined(USE_STDLIB_THREAD)
            size_t thread_id = omp_get_thread_num();
#elif defined(USE_STDLIB_THREAD)
            size_t thread_id = beg_end_pair_idx;
#else
            size_t thread_id = 0;
#endif
            auto & batcharg = batchargs[beg_end_pair_idx];
            batcharg.thread_id = thread_id;
            batcharg.hts_idx = sam_idxs[thread_id];
            batcharg.ref_faidx = ref_faidxs[thread_id];
            batcharg.sr = srs[thread_id];

            std::pair<unsigned int, unsigned int> beg_end_pair = beg_end_pair_vec[beg_end_pair_idx];
#if defined(USE_STDLIB_THREAD)
            std::thread athread([
                        &batcharg, allridx, beg_end_pair, beg_end_pair_idx, &tid_beg_end_e2e_tuple_vec, &tid_to_tname_tseqlen_tuple_vec, &tid_pos_symb_to_tkis1
                        ]() {
#endif
                    LOG(logINFO) << "Thread " << batcharg.thread_id << " will process the sub-chunk " << beg_end_pair_idx << " which ranges from " 
                            << beg_end_pair.first << " to " << beg_end_pair.second;
                    
                    for (unsigned int j = beg_end_pair.first; j < beg_end_pair.second; j++) {
                        batcharg.regionbatch_ordinal = j;
                        batcharg.regionbatch_tot_num = beg_end_pair.second;
                        batcharg.tid_beg_end_e2e_tuple = tid_beg_end_e2e_tuple_vec.at(allridx + j);
                        batcharg.tname_tseqlen_tuple = tid_to_tname_tseqlen_tuple_vec.at(std::get<0>(batcharg.tid_beg_end_e2e_tuple));
                        process_batch(batcharg, tid_pos_symb_to_tkis1);
                    }
#if defined(USE_STDLIB_THREAD)
            });
            threads.push_back(std::move(athread));
#endif
        }
#if defined(USE_STDLIB_THREAD)
        for (auto & t : threads) {
            t.join();
        }
#endif
        for (unsigned int beg_end_pair_idx = 0; beg_end_pair_idx < beg_end_pair_vec.size(); beg_end_pair_idx++) {
            if (batchargs[beg_end_pair_idx].outstring_allp.size() > 0) {
                clearstring<true>(fp_allp, batchargs[beg_end_pair_idx].outstring_allp); // empty string means end of file
            }
            if (batchargs[beg_end_pair_idx].outstring_pass.size() > 0) {
                clearstring<true>(fp_pass, batchargs[beg_end_pair_idx].outstring_pass); // empty string means end of file
            }
            all_vc_stats.update(batchargs[beg_end_pair_idx].vc_stats);
        }
        read_bam_thread.join(); // end this iter
        for (auto tid_pos_symb_to_tkis1_pair: tid_pos_symb_to_tkis1) {
            for (auto tki : tid_pos_symb_to_tkis1_pair.second) {
                if (NULL != tki.bcf1_record) {
                    bcf_destroy(tki.bcf1_record); 
                }
            }
        }
        autoswap(tid_beg_end_e2e_tuple_vec1, tid_beg_end_e2e_tuple_vec2);
        autoswap(tid_pos_symb_to_tkis1, tid_pos_symb_to_tkis2);
    }
    if (NOT_PROVIDED != paramset.vc_stats_fname) {
        std::ofstream vc_stats_ofstream(paramset.vc_stats_fname.c_str());
        all_vc_stats.write_tsv(vc_stats_ofstream);
    } else {
        all_vc_stats.write_tsv(std::cerr);
    }
    clearstring<true>(fp_allp, std::string("")); // write end of file
    clearstring<true>(fp_pass, std::string(""), is_vcf_out_pass_to_stdout);
    bam_hdr_destroy(samheader);
    if (NULL != g_bcf_hdr) {
        bcf_hdr_destroy(g_bcf_hdr);
    }
    for (size_t i = 0; i < nidxs; i++) { 
        if (NULL != srs[i]) {
            bcf_sr_destroy(srs[i]);
        }
        if (NULL != ref_faidxs[i]) { 
            fai_destroy(ref_faidxs[i]); 
        }
        if (NULL != sam_idxs[i]) {
            hts_idx_destroy(sam_idxs[i]);
        }
        if (NULL != samfiles[i]) {
            sam_close(samfiles[i]);
        }
    }
    // bgzf_flush is internally called by bgzf_close
    if (fp_allp) {
        int closeresult = bgzf_close(fp_allp);
        if (closeresult != 0) {
            LOG(logERROR) << "Unable to close the bgzip file " << paramset.vcf_output_fname;
        }
    }
    if (fp_pass) {
        int closeresult = bgzf_close(fp_pass);
        if (closeresult != 0) {
            LOG(logERROR) << "Unable to close the bgzip file " << paramset.vcf_output_fname;
        }
    }
    std::clock_t c_end = std::clock();
    auto t_end = std::chrono::high_resolution_clock::now();
 
    std::cerr << std::fixed << std::setprecision(2) << "CPU time used: "
              << 1.0 * (c_end-c_start) / CLOCKS_PER_SEC << " seconds\n"
              << "Wall clock time passed: "
              << std::chrono::duration<double>(t_end-t_start).count()
              << " seconds\n";
    return 0;
}

