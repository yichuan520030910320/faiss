/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// -*- c++ -*-

#include <faiss/IndexNSG.h>

#include <cinttypes>
#include <memory>

#include <faiss/IndexFlat.h>
#include <faiss/IndexNNDescent.h>
#include <faiss/impl/AuxIndexStructures.h>
#include <faiss/impl/FaissAssert.h>
#include <faiss/utils/distances.h>

namespace faiss {

using namespace nsg;

/**************************************************************
 * IndexNSG implementation
 **************************************************************/

IndexNSG::IndexNSG(int d, int R, MetricType metric) : Index(d, metric), nsg(R) {
    nndescent_L = GK + 50;
}

IndexNSG::IndexNSG(Index* storage, int R)
        : Index(storage->d, storage->metric_type),
          nsg(R),
          storage(storage),
          build_type(1) {
    nndescent_L = GK + 50;
}

IndexNSG::~IndexNSG() {
    if (own_fields) {
        delete storage;
    }
}

void IndexNSG::train(idx_t n, const float* x) {
    FAISS_THROW_IF_NOT_MSG(
            storage,
            "Please use IndexNSGFlat (or variants) instead of IndexNSG directly");
    // nsg structure does not require training
    storage->train(n, x);
    is_trained = true;
}

void IndexNSG::search(
        idx_t n,
        const float* x,
        idx_t k,
        float* distances,
        idx_t* labels,
        const SearchParameters* params) const {
    FAISS_THROW_IF_NOT_MSG(
            !params, "search params not supported for this index");
    FAISS_THROW_IF_NOT_MSG(
            storage,
            "Please use IndexNSGFlat (or variants) instead of IndexNSG directly");

    int L = std::max(nsg.search_L, (int)k); // in case of search L = -1
    idx_t check_period = InterruptCallback::get_period_hint(d * L);

    int ndis = 0;

    for (idx_t i0 = 0; i0 < n; i0 += check_period) {
        idx_t i1 = std::min(i0 + check_period, n);

#pragma omp parallel
        {
            VisitedTable vt(ntotal);

            std::unique_ptr<DistanceComputer> dis(
                    storage_distance_computer(storage));

#pragma omp for reduction(+ : ndis)
            for (idx_t i = i0; i < i1; i++) {
                idx_t* idxi = labels + i * k;
                float* simi = distances + i * k;
                dis->set_query(x + i * d);

                NSGStats stats = nsg.search(*dis, k, idxi, simi, vt);
                ndis += stats.ndis;

                vt.advance();
            }
        }
        InterruptCallback::check();
    }

    if (is_similarity_metric(metric_type)) {
        // we need to revert the negated distances
        for (size_t i = 0; i < k * n; i++) {
            distances[i] = -distances[i];
        }
    }

    nsg_stats.ndis += ndis;
}

void IndexNSG::build(idx_t n, const float* x, idx_t* knn_graph, int GK_2) {
    FAISS_THROW_IF_NOT_MSG(
            storage,
            "Please use IndexNSGFlat (or variants) instead of IndexNSG directly");
    FAISS_THROW_IF_NOT_MSG(
            !is_built && ntotal == 0, "The IndexNSG is already built");

    storage->add(n, x);
    ntotal = storage->ntotal;

    // check the knn graph
    check_knn_graph(knn_graph, n, GK_2);

    const nsg::Graph<idx_t> knng(knn_graph, n, GK_2);
    nsg.build(storage, n, knng, verbose);
    is_built = true;
}

void IndexNSG::add(idx_t n, const float* x) {
    FAISS_THROW_IF_NOT_MSG(
            storage,
            "Please use IndexNSGFlat (or variants) "
            "instead of IndexNSG directly");
    FAISS_THROW_IF_NOT(is_trained);

    FAISS_THROW_IF_NOT_MSG(
            !is_built && ntotal == 0,
            "NSG does not support incremental addition");

    std::vector<idx_t> knng;
    if (verbose) {
        printf("IndexNSG::add %zd vectors\n", size_t(n));
    }

    if (build_type == 0) { // build with brute force search

        if (verbose) {
            printf("  Build knn graph with brute force search on storage index\n");
        }

        storage->add(n, x);
        ntotal = storage->ntotal;
        FAISS_THROW_IF_NOT(ntotal == n);

        knng.resize(ntotal * (GK + 1));
        storage->assign(ntotal, x, knng.data(), GK + 1);

        // Remove itself
        // - For metric distance, we just need to remove the first neighbor
        // - But for non-metric, e.g. inner product, we need to check
        // - each neighbor
        if (storage->metric_type == METRIC_INNER_PRODUCT) {
            for (idx_t i = 0; i < ntotal; i++) {
                int count = 0;
                for (int j = 0; j < GK + 1; j++) {
                    idx_t id = knng[i * (GK + 1) + j];
                    if (id != i) {
                        knng[i * GK + count] = id;
                        count += 1;
                    }
                    if (count == GK) {
                        break;
                    }
                }
            }
        } else {
            for (idx_t i = 0; i < ntotal; i++) {
                memmove(knng.data() + i * GK,
                        knng.data() + i * (GK + 1) + 1,
                        GK * sizeof(idx_t));
            }
        }

    } else if (build_type == 1) { // build with NNDescent
        idx_t knng_size_nnd = (size_t)n * GK;
        if (verbose) {
            printf("  Resizing knng vector (n=%" PRId64 ", GK=%d)\n", n, GK);
        }
        try {
            knng.resize(knng_size_nnd);
        } catch (const std::bad_alloc& e) {
            fprintf(stderr,
                    "FATAL: Failed to allocate memory for knng (%" PRId64
                    " * %d * %zu bytes). std::bad_alloc: %s\n",
                    n,
                    GK,
                    sizeof(idx_t),
                    e.what());
            FAISS_THROW_FMT(
                    "Memory allocation failed for knng vector of size %zd",
                    knng_size_nnd);
        }
        if (verbose) {
            printf("  Successfully resized knng vector.\n");
        }

        IndexNNDescent index(storage, GK);
        index.nndescent.S = nndescent_S;
        index.nndescent.R = nndescent_R;
        index.nndescent.L = std::max(nndescent_L, GK + 50);
        index.nndescent.iter = nndescent_iter;
        index.verbose = verbose;

        if (verbose) {
            printf("  Build knn graph with NNdescent S=%d R=%d L=%d niter=%d\n",
                   index.nndescent.S,
                   index.nndescent.R,
                   index.nndescent.L,
                   index.nndescent.iter);
        }

        // prevent IndexNSG from deleting the storage
        index.own_fields = false;

        index.add(n, x);

        // storage->add is already implicit called in IndexNSG.add
        ntotal = storage->ntotal;
        FAISS_THROW_IF_NOT(ntotal == n);

        // cast from idx_t to int
        printf("index.nndescent.final_graph.data() = %p\n",
               index.nndescent.final_graph.data());
        const int* knn_graph = index.nndescent.final_graph.data();
#pragma omp parallel for
        for (idx_t i = 0; i < ntotal * GK; i++) {
            knng[i] = knn_graph[i];
        }
    } else {
        FAISS_THROW_MSG("build_type should be 0 or 1");
    }

    if (verbose) {
        printf("  Check the knn graph\n");
    }

    // check the knn graph
    check_knn_graph(knng.data(), n, GK);

    if (verbose) {
        printf("  nsg building\n");
    }

    const nsg::Graph<idx_t> knn_graph(knng.data(), n, GK);
    nsg.build(storage, n, knn_graph, verbose);
    is_built = true;
}

void IndexNSG::reset() {
    nsg.reset();
    storage->reset();
    ntotal = 0;
    is_built = false;
}

void IndexNSG::reconstruct(idx_t key, float* recons) const {
    storage->reconstruct(key, recons);
}

void IndexNSG::check_knn_graph(const idx_t* knn_graph, idx_t n, int K) const {
    idx_t total_count = 0;

#pragma omp parallel for reduction(+ : total_count)
    for (idx_t i = 0; i < n; i++) {
        int count = 0;
        for (int j = 0; j < K; j++) {
            idx_t id = knn_graph[i * K + j];
            if (id < 0 || id >= n || id == i) {
                count += 1;
            }
        }
        total_count += count;
    }

    if (total_count > 0) {
        fprintf(stderr,
                "WARNING: the input knn graph "
                "has %" PRId64 " invalid entries\n",
                total_count);
    }
    FAISS_THROW_IF_NOT_MSG(
            total_count < n / 10,
            "There are too much invalid entries in the knn graph. "
            "It may be an invalid knn graph.");
}

/**************************************************************
 * IndexNSGFlat implementation
 **************************************************************/

IndexNSGFlat::IndexNSGFlat() {
    is_trained = true;
}

IndexNSGFlat::IndexNSGFlat(int d, int R, MetricType metric)
        : IndexNSG(new IndexFlat(d, metric), R) {
    own_fields = true;
    is_trained = true;
}

/**************************************************************
 * IndexNSGPQ implementation
 **************************************************************/

IndexNSGPQ::IndexNSGPQ() = default;

IndexNSGPQ::IndexNSGPQ(int d, int pq_m, int M, int pq_nbits)
        : IndexNSG(new IndexPQ(d, pq_m, pq_nbits), M) {
    own_fields = true;
    is_trained = false;
}

void IndexNSGPQ::train(idx_t n, const float* x) {
    IndexNSG::train(n, x);
    (dynamic_cast<IndexPQ*>(storage))->pq.compute_sdc_table();
}

/**************************************************************
 * IndexNSGSQ implementation
 **************************************************************/

IndexNSGSQ::IndexNSGSQ(
        int d,
        ScalarQuantizer::QuantizerType qtype,
        int M,
        MetricType metric)
        : IndexNSG(new IndexScalarQuantizer(d, qtype, metric), M) {
    is_trained = this->storage->is_trained;
    own_fields = true;
}

IndexNSGSQ::IndexNSGSQ() = default;

} // namespace faiss
