/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// -*- c++ -*-

#pragma once

#include <atomic>
#include <vector>

#include <faiss/IndexFlat.h>
#include <faiss/IndexPQ.h>
#include <faiss/IndexScalarQuantizer.h>
#include <faiss/impl/HNSW.h>
#include <faiss/utils/utils.h>

namespace faiss {

struct IndexHNSW;

/** The HNSW index is a normal random-access index with a HNSW
 * link structure built on top */

struct IndexHNSW : Index {
    typedef HNSW::storage_idx_t storage_idx_t;

    // the link structure
    HNSW hnsw;

    // the sequential storage
    bool own_fields = false;
    Index* storage = nullptr;

    // When set to false, level 0 in the knn graph is not initialized.
    // This option is used by GpuIndexCagra::copyTo(IndexHNSWCagra*)
    // as level 0 knn graph is copied over from the index built by
    // GpuIndexCagra.
    bool init_level0 = true;

    // When set to true, all neighbors in level 0 are filled up
    // to the maximum size allowed (2 * M). This option is used by
    // IndexHHNSWCagra to create a full base layer graph that is
    // used when GpuIndexCagra::copyFrom(IndexHNSWCagra*) is invoked.
    bool keep_max_size_level0 = false;

    // ---- Modifications for atomic counter ----
    // Instead of a direct std::atomic member (which can't be copied),
    // use a pointer to an atomic. The pointer can be copied, and each
    // copy gets its own atomic counter.
    mutable std::atomic<size_t>* fetch_count_ptr = nullptr;

    bool is_recompute = false;

    // ---- Experimental top degree disk read feature ----
    std::string experimental_disk_storage_path = "";
    off_t experimental_disk_data_offset = -1;

    explicit IndexHNSW(
            int d = 0,
            int M = 32,
            MetricType metric = METRIC_L2,
            int M0 = -1);
    explicit IndexHNSW(Index* storage, int M = 32, int M0 = -1);

    ~IndexHNSW() override;

    void add(idx_t n, const float* x) override;

    /// Trains the storage if needed
    void train(idx_t n, const float* x) override;

    /// entry point for search
    void search(
            idx_t n,
            const float* x,
            idx_t k,
            float* distances,
            idx_t* labels,
            const SearchParameters* params = nullptr) const override;

    void range_search(
            idx_t n,
            const float* x,
            float radius,
            RangeSearchResult* result,
            const SearchParameters* params = nullptr) const override;

    void reconstruct(idx_t key, float* recons) const override;

    void reset() override;

    void shrink_level_0_neighbors(int size);

    /** Perform search only on level 0, given the starting points for
     * each vertex.
     *
     * @param search_type 1:perform one search per nprobe, 2: enqueue
     *                    all entry points
     */
    void search_level_0(
            idx_t n,
            const float* x,
            idx_t k,
            const storage_idx_t* nearest,
            const float* nearest_d,
            float* distances,
            idx_t* labels,
            int nprobe = 1,
            int search_type = 1,
            const SearchParameters* params = nullptr) const;

    /// alternative graph building
    void init_level_0_from_knngraph(int k, const float* D, const idx_t* I);

    /// alternative graph building
    void init_level_0_from_entry_points(
            int npt,
            const storage_idx_t* points,
            const storage_idx_t* nearests);

    // reorder links from nearest to farthest
    void reorder_links();

    void link_singletons();

    void permute_entries(const idx_t* perm);

    DistanceComputer* get_distance_computer() const override;

    /// Get the total number of vector fetches performed during the last search.
    size_t get_last_total_fetch_count() const;

    void save_edge_stats(const char* filename) const;
};

/** Flat index topped with with a HNSW structure to access elements
 *  more efficiently.
 */

struct IndexHNSWFlat : IndexHNSW {
    IndexHNSWFlat();
    IndexHNSWFlat(int d, int M, MetricType metric = METRIC_L2, int M0 = -1);
};

/** PQ index topped with with a HNSW structure to access elements
 *  more efficiently.
 */
struct IndexHNSWPQ : IndexHNSW {
    IndexHNSWPQ();
    IndexHNSWPQ(
            int d,
            int pq_m,
            int M,
            int pq_nbits = 8,
            MetricType metric = METRIC_L2);
    void train(idx_t n, const float* x) override;
};

/** SQ index topped with a HNSW structure to access elements
 *  more efficiently.
 */
struct IndexHNSWSQ : IndexHNSW {
    IndexHNSWSQ();
    IndexHNSWSQ(
            int d,
            ScalarQuantizer::QuantizerType qtype,
            int M,
            MetricType metric = METRIC_L2);
};

/** 2-level code structure with fast random access
 */
struct IndexHNSW2Level : IndexHNSW {
    IndexHNSW2Level();
    IndexHNSW2Level(Index* quantizer, size_t nlist, int m_pq, int M);

    void flip_to_ivf();

    /// entry point for search
    void search(
            idx_t n,
            const float* x,
            idx_t k,
            float* distances,
            idx_t* labels,
            const SearchParameters* params = nullptr) const override;
};

struct IndexHNSWCagra : IndexHNSW {
    IndexHNSWCagra();
    IndexHNSWCagra(int d, int M, MetricType metric = METRIC_L2);

    /// When set to true, the index is immutable.
    /// This option is used to copy the knn graph from GpuIndexCagra
    /// to the base level of IndexHNSWCagra without adding upper levels.
    /// Doing so enables to search the HNSW index, but removes the
    /// ability to add vectors.
    bool base_level_only = false;

    /// When `base_level_only` is set to `True`, the search function
    /// searches only the base level knn graph of the HNSW index.
    /// This parameter selects the entry point by randomly selecting
    /// some points and using the best one.
    int num_base_level_search_entrypoints = 32;

    void add(idx_t n, const float* x) override;

    /// Initialize or re-initialize the atomic counter
    void init_counter() {
        // Delete any existing counter first
        if (fetch_count_ptr) {
            delete fetch_count_ptr;
        }
        // Create a new atomic counter initialized to 0
        fetch_count_ptr = new std::atomic<size_t>(0);
    }

    /// entry point for search
    void search(
            idx_t n,
            const float* x,
            idx_t k,
            float* distances,
            idx_t* labels,
            const SearchParameters* params = nullptr) const override;
};

} // namespace faiss
