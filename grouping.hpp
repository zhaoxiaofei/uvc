#ifndef grouping_hpp_INCLUDED
#define grouping_hpp_INCLUDED

#include "common.hpp"

#include "htslib/sam.h"

#include <cassert>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define logDEBUGx1 logDEBUG // logINFO

int 
bed_fname_to_contigs(
        std::vector<std::tuple<unsigned int, unsigned int, unsigned int, bool, unsigned int>> & tid_beg_end_e2e_vec,
        const std::string & bed_fname, const bam_hdr_t *bam_hdr);

struct SamIter {
    const std::string input_bam_fname;
    const std::string & tier1_target_region; 
    const std::string region_bed_fname;
    const unsigned int nthreads;
    samFile *sam_infile = NULL;
    bam_hdr_t *samheader = NULL;
    hts_idx_t *sam_idx = NULL; 
    hts_itr_t *sam_itr = NULL;
    
    unsigned int endingpos = UINT_MAX;
    unsigned int tid = UINT_MAX;
    unsigned int tbeg = -1;
    unsigned int tend = -1;
    uint64_t nreads = 0;
    uint64_t next_nreads = 0;
    bam1_t *alnrecord = bam_init1();
    
    std::vector<std::tuple<unsigned int, unsigned int, unsigned int, bool, unsigned int>> _tid_beg_end_e2e_vec;
    unsigned int _bedregion_idx = 0;
    
    SamIter(const std::string & in_bam_fname, 
            const std::string & tier1_target_reg, 
            const std::string & reg_bed_fname, 
            const unsigned int nt): 
            input_bam_fname(in_bam_fname), 
            tier1_target_region(tier1_target_reg), 
            region_bed_fname(reg_bed_fname), 
            nthreads(nt) {
        this->sam_infile = sam_open(input_bam_fname.c_str(), "r");
        if (NULL == this->sam_infile) {
            fprintf(stderr, "Failed to open the file %s!", input_bam_fname.c_str());
            abort();
        }
        this->samheader = sam_hdr_read(sam_infile);
        if (NULL == this->samheader) {
            fprintf(stderr, "Failed to read the header of the file %s!", input_bam_fname.c_str());
            abort();
        }
        if (NOT_PROVIDED != this->tier1_target_region) {
            this->sam_idx = sam_index_load(this->sam_infile, input_bam_fname.c_str());
            if (NULL == this->sam_idx) {
                fprintf(stderr, "Failed to load the index for the file %s!", input_bam_fname.c_str());
                abort();
            }
            this->sam_itr = sam_itr_querys(this->sam_idx, this->samheader, this->tier1_target_region.c_str());
            if (NULL == this->sam_itr) {
                fprintf(stderr, "Failed to load the region %s in the indexed file %s!", tier1_target_region.c_str(), input_bam_fname.c_str());
                abort();
            }
        }
        
        if (NOT_PROVIDED != this->region_bed_fname) {
            bed_fname_to_contigs(this->_tid_beg_end_e2e_vec, this->region_bed_fname, this->samheader); 
        }
    }
    ~SamIter() {
        bam_destroy1(alnrecord);
        if (NULL != sam_itr) { sam_itr_destroy(sam_itr); }
        if (NULL != sam_idx) { hts_idx_destroy(sam_idx); }
        bam_hdr_destroy(samheader);
        sam_close(sam_infile);
    }
    
    int 
    iternext(std::vector<std::tuple<unsigned int, unsigned int, unsigned int, bool, unsigned int>> & tid_beg_end_e2e_vec);
};

int
samfname_to_tid_to_tname_tseq_tup_vec(
        std::vector<std::tuple<std::string, unsigned int>> & tid_to_tname_tseqlen_tuple_vec, 
        const std::string & bam_input_fname);

int 
sam_fname_to_contigs(
        std::vector<std::tuple<unsigned int, unsigned int, unsigned int, bool, unsigned int>> & tid_beg_end_e2e_vec,
        std::vector<std::tuple<std::string, unsigned int>> & tid_to_tname_tlen_tuple_vec,
        const std::string & input_bam_fname, 
        const std::string & bed_fname);

struct _RevComplement {
    char data[128];
    char table16[16];
    _RevComplement() {
        for (int i = 0; i < 128; i++) {
            data[i] = (char)i; 
        }
        data['A'] = 'T';
        data['T'] = 'A'; 
        data['C'] = 'G'; 
        data['G'] = 'C'; 
        data['a'] = 't';
        data['t'] = 'a'; 
        data['c'] = 'g'; 
        data['g'] = 'c'; 
        for (int i = 0; i < 16; i++) {
            table16[i] = (char)i;
        }
        table16[1] = 8/1;
        table16[2] = 8/2;
        table16[4] = 8/4;
        table16[8] = 8/8;
    }
};

const _RevComplement THE_REV_COMPLEMENT;

template <class TContainer>
int 
revcompln(TContainer & str, size_t len) {
    for (size_t i = 0; i < len / 2; i++) {
        autoswap(str[i], str[len-1-i]);
    }
    for (size_t i = 0; i < len; i++) {
        str[i] = THE_REV_COMPLEMENT.data[str[i]];    
    }
    return 0;
}

template <class TContainer>
int 
revcompl(TContainer & str) {
    return revcompln(str, str.size());
}

int 
clean_fill_strand_umi_readset(
        std::vector<std::array<std::vector<std::vector<bam1_t *>>, 2>> &umi_strand_readset);

int 
fill_strand_umi_readset_with_strand_to_umi_to_reads(
        std::vector<std::pair<std::array<std::vector<std::vector<bam1_t *>>, 2>, int>> &umi_strand_readset,
        std::map<uint64_t, std::pair<std::array<std::map<uint64_t, std::vector<bam1_t *>>, 2>, int>> &umi_to_strand_to_reads,
        unsigned int baq_per_aligned_base);

std::array<unsigned int, 3>
bamfname_to_strand_to_familyuid_to_reads(
        std::map<uint64_t, std::pair<std::array<std::map<uint64_t, std::vector<bam1_t *>>, 2>, int>> &umi_to_strand_to_reads,
        unsigned int & extended_inclu_beg_pos,
        unsigned int & extended_exclu_end_pos,
        const std::string input_bam_fname, 
        unsigned int tid, 
        unsigned int fetch_tbeg, 
        unsigned int fetch_tend, 
        bool end2end, 
        unsigned int min_mapq, 
        unsigned int min_alnlen, 
        unsigned int regionbatch_ordinal, 
        unsigned int regionbatch_tot_num,
        const std::string UMI_STRUCT_STRING, 
        const hts_idx_t * hts_idx,
        const bool is_molecule_tag_enabled,
        const bool is_pair_end_merge_enabled, 
        bool disable_duplex,
        size_t thread_id,
        unsigned int dedup_center_mult,
        unsigned int dedup_amplicon_count_to_surrcount_frac,
        unsigned int dedup_yes_umi_2ends_peak_frac,
        unsigned int dedup_non_umi_2ends_peak_frac
        );
#endif

