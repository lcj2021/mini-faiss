#ifndef INDEX_IVFPQ_H
#define INDEX_IVFPQ_H

#include <iostream>
#include <iomanip>
#include <algorithm>
#include <cassert>
#include <unordered_set>
#include <fstream>
#include "util.h"
#include "quantizer.h"
#include "binary_io.h"
#include "distance.h"
#include <omp.h>

namespace Toy {

/**
 * Configuration structure
 * @param N_ the number of data
 * @param D_ the number of dimensions
 * @param W_ the number of bucket involed when searching is performed
 * @param L_ the expected number of candidates involed when searching is performed
 * @param kc, kp the number of coarse quantizer (nlist) and product quantizer's centers (1 << nbits). Default: 100, 256
 * @param mc, mp the number of subspace for coarse quantizer and product quantizer. mc must be 1
 * @param dc, dp the dimensions of subspace for coarse quantize and product quantizer. dc must be D_.  dp = D_ / mp
 * @param db_path path to the DB files
 * @param db_prefix the prefix of DB files
 */
struct IVFPQConfig {
	size_t N_, D_, W_, L_, kc, kp, mc, mp, dc, dp;
	// std::string db_prefix;
	// std::string db_path;
    IVFPQConfig(size_t N, size_t D, size_t W, size_t L, 
                size_t kc, size_t kp, 
                size_t mc, size_t mp, 
                size_t dc, size_t dp) : N_(N), D_(D), W_(W), L_(L), 
                kc(kc), kp(kp), mc(mc), mp(mp), dc(dc), dp(dp) {}
};

struct DistanceTable{
    // Helper structure. This is identical to vec<vec<float>> dt(M, vec<float>(Ks))
    DistanceTable() {}
    DistanceTable(size_t M, size_t Ks) : kp(Ks), data_(M * Ks) {}
    void set_value(size_t m, size_t ks, float val) {
        data_[m * kp + ks] = val;
    }
    float get_value(size_t m, size_t ks) const {
        return data_[m * kp + ks];
    }
    size_t kp;
    std::vector<float> data_;
};

class IndexIVFPQ {
public:
    IndexIVFPQ(const IVFPQConfig& cfg, size_t nq, bool verbose, bool write_trainset);

    void populate(const std::vector<float>& rawdata);
    void train(const std::vector<float>& traindata, int seed, bool need_split);

    std::pair<std::vector<size_t>, std::vector<float>> query(const std::vector<float>& query,
                                                             const std::vector<int>& gt,
                                                             int topk,
                                                             int L,
                                                             int id);
    void insert_ivf(const std::vector<float>& rawdata);
    void write_trainset(const std::string& dataset_name);

    DistanceTable DTable(const std::vector<float>& vec) const;
    float ADist(const DistanceTable& dtable, const std::vector<uint8_t>& code) const;
    float ADist(const DistanceTable& dtable, size_t list_no, size_t offset) const;
    float ADist(const DistanceTable& dtable, const std::vector<uint8_t>& flattened_codes, size_t n) const;
    std::pair<std::vector<size_t>, std::vector<float>> PairVectorToVectorPair(const std::vector<std::pair<size_t, float>>& pair_vec) const;

    std::vector<uint8_t> get_single_code(size_t list_no, size_t offset) const;
    // Given a long (N * M) codes, pick up n-th code
    template<typename T>
    const std::vector<T> nth_raw_vector(const std::vector<T>& long_code, size_t n) const;
    // Given a long (N * M) codes, pick up m-th element from n-th code
    uint8_t nth_vector_mth_element(const std::vector<uint8_t>& long_code, size_t n, size_t m) const;
    uint8_t nth_vector_mth_element(size_t list_no, size_t offset, size_t m) const;
    // Member variables
    size_t N_, D_, W_, L_, nq, kc, kp, mc, mp, dc, dp;
    bool verbose_, write_trainset_, is_trained_;

    std::unique_ptr<Quantizer::Quantizer> cq_, pq_;

    std::vector<std::vector<float>> centers_cq_;
    std::vector<int> labels_cq_;

    std::vector<std::vector<std::vector<float>>> centers_pq_;
    std::vector<std::vector<int>> labels_pq_;


    std::vector<std::vector<uint8_t>> db_codes_; // binary codes, size nlist
    std::vector<std::vector<int>> posting_lists_;  // (NumList, any)
    std::vector<std::vector<float>> posting_dist_lists_;  // (NumList, any)

    std::vector<float> L, R, distance_, farthest_;
    std::vector<int> querycodes_, centroidcodes_, distribution_;
};


IndexIVFPQ::IndexIVFPQ(const IVFPQConfig& cfg, size_t nq, bool verbose, bool write_trainset)
    : N_(cfg.N_), D_(cfg.D_), W_(cfg.W_), L_(cfg.L_), nq(nq), 
    kc(cfg.kc), kp(cfg.kp), mc(cfg.mc), mp(cfg.mp), dc(cfg.dc), dp(cfg.dp)
{
    verbose_ = verbose;
    write_trainset_ = write_trainset;
    assert(dc == D_ && mc == 1);

    cq_ = nullptr;
    pq_ = nullptr;

    if (write_trainset) {
        L.resize(nq * W_);
        R.resize(nq * W_);
        distance_.resize(nq * W_);

        farthest_.resize(kc);
        distribution_.resize(kc * 20);

        querycodes_.resize(nq * mp);
        centroidcodes_.resize(kc * mp);
    }

    if (verbose_) {
        // Check which SIMD functions are used. See distance.h for this global variable.
        std::cout << "SIMD support: " << g_simd_architecture << std::endl;
    }
}

void 
IndexIVFPQ::train(const std::vector<float>& rawdata, int seed, bool need_split)
{
    std::vector<float>* traindata = nullptr;
    size_t Nt_ = rawdata.size() / D_;
    if (need_split) {
        Nt_ = std::min(N_, (size_t)200'000);
        // Nt_ = std::min(N_, (size_t)10'000);      // for test only
        std::vector<size_t> ids(N_);
        std::iota(ids.begin(), ids.end(), 0);
        std::mt19937 default_random_engine(seed);
        std::shuffle(ids.begin(), ids.end(), default_random_engine);
        traindata = new std::vector<float>();
        traindata->reserve(Nt_ * D_);
        for (size_t k = 0; k < Nt_; ++k) {
            size_t id = ids[k];
            traindata->insert(traindata->end(), rawdata.begin() + id * D_, rawdata.begin() + (id + 1) * D_);
        }
    } else {
        traindata = const_cast<std::vector<float>*>(&rawdata);
    }

    cq_ = std::make_unique<Quantizer::Quantizer>(D_, Nt_, mc, kc, true);
    cq_->fit(*traindata, 12, seed);
    centers_cq_ = cq_->get_centroids()[0];      // Because mc == 1
    labels_cq_ = cq_->get_assignments()[0];

    // std::vector<size_t> labels_cnt(kc);
    // for (auto label : labels_cq_) {
    //     labels_cnt[label] ++;
    // }
    // std::sort(labels_cnt.begin(), labels_cnt.end());
    // for (auto cnt : labels_cnt) {
    //     std::cout << cnt << '\n';
    // }

    pq_ = std::make_unique<Quantizer::Quantizer>(D_, Nt_, mp, kp, true);
    pq_->fit(*traindata, 6, seed);
    centers_pq_ = pq_->get_centroids();
    labels_pq_ = pq_->get_assignments();

    if (traindata != &rawdata) delete traindata;
    is_trained_ = true;
}

void 
IndexIVFPQ::insert_ivf(const std::vector<float>& rawdata)
{
    const auto& pqcodes = pq_->encode(rawdata);

// #pragma omp parallel for
    for (size_t n = 0; n < N_; ++n) {
        int id = cq_->predict_one(nth_raw_vector(rawdata, n), 0);
        // #pragma omp critical
        {
            posting_lists_[id].emplace_back(n);
            posting_dist_lists_[id].emplace_back(0);
        }
    }

#pragma omp parallel for
    for (size_t no = 0; no < kc; ++no) {
        auto& plist = posting_lists_[no];
        const auto& center = centers_cq_[no];
        auto& pdlist = posting_dist_lists_[no];

        /**
         * Which distance should be used? Real distance or PQ distance? 
         * @todo Finish Quantizer.asynm_dist()
         * @todo Finish Quantizer.synm_dist()
         * */ 
        std::vector<size_t> indices(plist.size());
        std::iota(indices.begin(), indices.end(), 0);
        for (size_t i = 0; i < pdlist.size(); ++i) {
            pdlist[i] = fvec_L2sqr(center.data(), nth_raw_vector(rawdata, plist[i]).data(), D_);
        }

        std::sort(indices.begin(), indices.end(), [&](size_t a, size_t b) {
            return pdlist[a] < pdlist[b];
        });
        auto sorted_pdlist = pdlist;
        auto sorted_plist = plist;
        for (size_t i = 0; i < plist.size(); ++i) {
            sorted_pdlist[i] = pdlist[indices[i]];
            sorted_plist[i] = plist[indices[i]];
        }
        std::swap(pdlist, sorted_pdlist);
        std::swap(plist, sorted_plist);
        std::vector<float>().swap(sorted_pdlist);
        std::vector<int>().swap(sorted_plist);

        for (const auto& id : plist) {
            const auto& nth_code = pqcodes[id];
            db_codes_[no].insert(db_codes_[no].end(), nth_code.begin(), nth_code.end());
        }
    }

    if (write_trainset_) {
        for (int no = 0; no < kc; ++no) {
            const auto& centroidword = pq_->encode(centers_cq_[no]);
            // printf("!!!!!!!!!!!!!!!!!!!%d\n", no);
            // for (auto x : centroidword[0]) std::cout << (int)x << ' '; std::cout << '\n';
            for (size_t i = 0; i < centroidword[0].size(); ++i) {
                centroidcodes_[no * mp + i] = (int)centroidword[0][i];
            }
            const auto& pdlist = posting_dist_lists_[no];

            std::vector<float> dist_bin(21);
            std::vector<size_t> count_bin(20);
            float interval = pdlist.back() / 20;
            for (size_t i = 0; i < 21; ++ i) {
                dist_bin[i] = i * interval;
            }
            size_t idx = 1;
            for (const auto& dist : pdlist) {
                if (dist > dist_bin[idx]) {
                    idx ++;
                } else {
                }
                count_bin[idx - 1] ++;
            }
            for (size_t i = 0; i < 20; ++i) {
                distribution_[no * 20 + i] = count_bin[i];
            }
            farthest_[no] = pdlist.back();
        }
    }
}


void 
IndexIVFPQ::populate(const std::vector<float>& rawdata)
{
    assert(rawdata.size() / D_ == N_);
    if (!is_trained_ || centers_cq_.empty()) {
        std::cerr << "Error. train() must be called before running populate(vecs=X).\n";
        throw;
    }

    // ===== (1) Encode rawdata to PQcodes =====
//     std::vector<std::vector<uint8_t>> pqcodes(N_, std::vector<uint8_t>(mp));
// #pragma omp parallel for
//     for (size_t n = 0; n < N_; ++n) {
//         pqcodes[n] = encode(nth_raw_vector(rawdata, n));
//     }

    // ===== (2) Update posting lists =====
    if (verbose_) { std::cout << "Start to update posting lists" << std::endl; }

    posting_lists_.clear();
    posting_lists_.resize(kc);
    db_codes_.clear();
    db_codes_.resize(kc);
    posting_dist_lists_.clear();
    posting_dist_lists_.resize(kc);

    for (auto& posting_list : posting_lists_) {
        posting_list.reserve(N_ / kc);  // Roughly malloc
    }
    for (auto& posting_dist_list : posting_dist_lists_) {
        posting_dist_list.reserve(N_ / kc);  // Roughly malloc
    }
    for (auto& code : db_codes_) {
        code.reserve(N_ / kc);  // Roughly malloc
    }
    insert_ivf(rawdata);

    if (verbose_) {
        std::cout << N_ << " new vectors are added." << std::endl;
    }
}

std::pair<std::vector<size_t>, std::vector<float> > 
IndexIVFPQ::query(const std::vector<float>& query,
                                            const std::vector<int>& gt,
                                            int topk,
                                            int L,
                                            int id)
{
    // ===== (1) Create dtable =====
    DistanceTable dtable = DTable(query);


    // ===== (2) Compare to coarse centers and sort the results =====
    std::vector<std::pair<size_t, float>> scores_coarse(centers_cq_.size());
//#pragma omp parallel for
    for (size_t no = 0; no < kc; ++no) {
        scores_coarse[no] = {no, fvec_L2sqr(query.data(), centers_cq_[no].data(), D_)};
    }

    // Timer timer1, timer2;
    // ===== (3) Partial sort the coarse results. =====
    // timer1.start();
    std::partial_sort(scores_coarse.begin(), scores_coarse.begin() + W_, scores_coarse.end(),
                      [](const std::pair<size_t, float>& a, const std::pair<size_t, float>& b){return a.second < b.second;});
    // timer1.stop();

    std::unordered_set<int> gt_set;
    if (write_trainset_) {
        gt_set = std::unordered_set<int>(gt.begin(), gt.end());
        const auto& queryword = pq_->encode(query);
        assert(queryword[0].size() == mp);
        for (size_t i = 0; i < queryword[0].size(); ++i) {
            querycodes_[id * mp + i] = (int)queryword[0][i];
        }
    }

    // ===== (4) Traverse posting list =====
    // timer2.start();
    std::vector<std::pair<size_t, float>> scores;
    scores.reserve(L);
    int coarse_cnt = 0;
    for (const auto& score_coarse : scores_coarse) {
        size_t no = score_coarse.first;
        size_t bl = posting_lists_[no].size(), br = 0;
        size_t hit_count = 0, posting_lists_len = posting_lists_[no].size();

        // [TODO] parallelized
        // Doesn't benefit 
// #pragma omp parallel for if(posting_lists_len > 10000)
        for (size_t idx = 0; idx < posting_lists_len; ++idx) {
            const auto& n = posting_lists_[no][idx];
            if (write_trainset_ && gt_set.count(n)) {
                bl = std::min(bl, idx);
                br = std::max(br, idx);
                hit_count ++;
            }
            // ===== (6) Evaluate n =====
            /**
             * scores.emplace_back(n, ADist(dtable, flattened_codes_, n));
             * 3x Faster than ADist(dtable, flattened_codes_, n)
             * Memory locality!
            */
            scores.emplace_back(n, ADist(dtable, no, idx));
        }

        // std::cout << std::fixed << std::setprecision(2)
        //             << (double)bl / posting_lists_[no].size() << ' ' 
        //             << (double)br / posting_lists_[no].size() << "\t"
        //             << hit_count << ' ' << score_coarse.second << '\n';
        if (write_trainset_) {
            this->L[id * W_ + coarse_cnt] = (float)bl / posting_lists_[no].size();
            this->R[id * W_ + coarse_cnt] = (float)br / posting_lists_[no].size();
            this->distance_[id * W_ + coarse_cnt] = score_coarse.second;
        }

        coarse_cnt++;
        if ( (size_t) coarse_cnt == W_ && scores.size() >= (unsigned long) topk) {
            // ===== (8) Sort them =====
            topk = std::min(topk, (int)scores.size());
            std::partial_sort(scores.begin(), scores.begin() + topk, scores.end(),
                                [](const std::pair<size_t, float>& a, const std::pair<size_t, float>& b){return a.second < b.second;});
            scores.resize(topk);
            scores.shrink_to_fit();
            // for (auto [id, dist] : scores) {
            //     std::cout << id << ' ' << dist << '\n';
            // }

            // std::cout << '\n';
            // timer2.stop();
            // std::cout << timer1.get_time() << ' ' << timer2.get_time() << " | " 
            //             << timer2.get_time() / (timer1.get_time() + timer2.get_time()) << '\n';
            return PairVectorToVectorPair(scores);  // return pair<vec, vec>
        }
    }
    return std::pair<std::vector<size_t>, std::vector<float>>({}, {});
}

void 
IndexIVFPQ::write_trainset(const std::string& dataset_name)
{
    if (!write_trainset_) {
        std::cerr << "Write trainset flag not set!" << std::endl;
        return;
    }
    std::string f_suffix = ".fvecs", i_suffix = ".ivecs";
    write_to_file_binary(L, {nq, W_}, dataset_name + "_l" + f_suffix);
    write_to_file_binary(R, {nq, W_}, dataset_name + "_r" + f_suffix);
    write_to_file_binary(distance_, {nq, W_}, dataset_name + "_distance" + f_suffix);

    write_to_file_binary(farthest_, {kc, 1}, dataset_name + "_farthest" + f_suffix);
    write_to_file_binary(distribution_, {kc, 20}, dataset_name + "_distribution" + i_suffix);

    write_to_file_binary(querycodes_, {nq, mp}, dataset_name + "_querycodes" + i_suffix);
    write_to_file_binary(centroidcodes_, {kc, mp}, dataset_name + "_centroidcodes" + i_suffix);
}

DistanceTable 
IndexIVFPQ::DTable(const std::vector<float>& vec) const
{
    const auto& v = vec;
    size_t Ds = centers_pq_[0][0].size();
    assert((size_t) v.size() == mp * Ds);
    DistanceTable dtable(mp, kp);
    for (size_t m = 0; m < mp; ++m) {
        for (size_t ks = 0; ks < kp; ++ks) {
            dtable.set_value(m, ks, fvec_L2sqr(&(v[m * Ds]), centers_pq_[m][ks].data(), Ds));
        }
    }
    return dtable;
}

float 
IndexIVFPQ::ADist(const DistanceTable& dtable, const std::vector<uint8_t>& code) const
{
    assert(code.size() == mp);
    float dist = 0;
    for (size_t m = 0; m < mp; ++m) {
        uint8_t ks = code[m];
        dist += dtable.get_value(m, ks);
    }
    return dist;
}

float 
IndexIVFPQ::ADist(const DistanceTable& dtable, const std::vector<uint8_t>& flattened_codes, size_t n) const
{
    float dist = 0;
    for (size_t m = 0; m < mp; ++m) {
        uint8_t ks = nth_vector_mth_element(flattened_codes, n, m);
        dist += dtable.get_value(m, ks);
    }
    return dist;
}

float 
IndexIVFPQ::ADist(const DistanceTable& dtable, size_t list_no, size_t offset) const
{
    float dist = 0;
    for (size_t m = 0; m < mp; ++m) {
        uint8_t ks = nth_vector_mth_element(list_no, offset, m);
        dist += dtable.get_value(m, ks);
    }
    return dist;
}

std::pair<std::vector<size_t>, std::vector<float> > 
IndexIVFPQ::PairVectorToVectorPair(const std::vector<std::pair<size_t, float> >& pair_vec) const
{
    std::pair<std::vector<size_t>, std::vector<float>> vec_pair(std::vector<size_t>(pair_vec.size()), std::vector<float>(pair_vec.size()));
    for(size_t n = 0, N = pair_vec.size(); n < N; ++n) {
        vec_pair.first[n] = pair_vec[n].first;
        vec_pair.second[n] = pair_vec[n].second;
    }
    return vec_pair;
}

std::vector<uint8_t> 
IndexIVFPQ::get_single_code(size_t list_no, size_t offset) const
{
    return std::vector<uint8_t>(db_codes_[list_no].begin() + offset * mp, 
                            db_codes_[list_no].begin() + (offset + 1) * mp);
}

template<typename T>
const std::vector<T> 
IndexIVFPQ::nth_raw_vector(const std::vector<T>& long_code, size_t n) const
{
    return std::vector<T>(long_code.begin() + n * D_, long_code.begin() + (n + 1) * D_);
}

uint8_t 
IndexIVFPQ::nth_vector_mth_element(const std::vector<uint8_t>& long_code, size_t n, size_t m) const
{
    return long_code[ n * mp + m];
}

uint8_t 
IndexIVFPQ::nth_vector_mth_element(size_t list_no, size_t offset, size_t m) const
{
    return db_codes_[list_no][ offset * mp + m];
}

} // namespace Toy


#endif