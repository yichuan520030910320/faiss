/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include <omp.h>

#include <faiss/Index.h>
#include <faiss/impl/AuxIndexStructures.h>
#include <faiss/impl/FaissAssert.h>
#include <faiss/utils/Heap.h>
#include <faiss/utils/random.h>

namespace faiss {

/** Implementation of the Navigating Spreading-out Graph (NSG)
 * datastructure.
 *
 * Fast Approximate Nearest Neighbor Search With The
 * Navigating Spreading-out Graph
 *
 *  Cong Fu, Chao Xiang, Changxu Wang, Deng Cai, VLDB 2019
 *
 * This implementation is heavily influenced by the NSG
 * implementation by ZJULearning Group
 * (https://github.com/zjulearning/nsg)
 *
 * The NSG object stores only the neighbor link structure, see
 * IndexNSG.h for the full index object.
 */

struct DistanceComputer; // from AuxIndexStructures

namespace nsg {

struct Neighbor;
struct Node;

/***********************************************************
 * Graph structure to store a graph.
 *
 * It is represented by an adjacency matrix `data`, where
 * data[i, j] is the j-th neighbor of node i.
 ***********************************************************/

template <class node_t>
struct Graph {
    node_t* data;    ///< the flattened adjacency matrix, size N-by-K
    uint64_t K;      ///< nb of neighbors per node
    int N;           ///< total nb of nodes
    bool own_fields; ///< the underlying data owned by itself or not

    // construct from a known graph
    Graph(node_t* data, int N, uint64_t K)
            : data(data), K(K), N(N), own_fields(false) {}

    // construct an empty graph
    // NOTE: the newly allocated data needs to be destroyed at destruction time
    Graph(int N, uint64_t K) : K(K), N(N), own_fields(true) {
        data = new node_t[N * K];
    }

    // copy constructor
    Graph(const Graph& g) : Graph(g.N, g.K) {
        memcpy(data, g.data, N * K * sizeof(node_t));
    }

    // release the allocated memory if needed
    virtual ~Graph() {
        if (own_fields) {
            delete[] data;
        }
    }

    // access the j-th neighbor of node i
    inline node_t at(int i, int j) const {
        return data[i * K + j];
    }

    // access the j-th neighbor of node i by reference
    inline node_t& at(int i, int j) {
        return data[i * K + j];
    }

    // get all neighbors of node i (used during search only)
    virtual size_t get_neighbors(int i, node_t* neighbors) const {
        for (int j = 0; j < K; j++) {
            if (data[i * K + j] < 0) {
                return j;
            }
            neighbors[j] = data[i * K + j];
        }
        return K;
    }
};

DistanceComputer* storage_distance_computer(const Index* storage);

} // namespace nsg

struct NSGStats {
    int ndis = 0;
};

FAISS_API extern NSGStats nsg_stats;

struct NSG {
    /// internal storage of vectors (32 bits: this is expensive)
    using storage_idx_t = int32_t;
    using Node = nsg::Node;
    using Neighbor = nsg::Neighbor;

    int ntotal = 0; ///< nb of nodes

    // construction-time parameters
    int R; ///< nb of neighbors per node
    int L; ///< length of the search path at construction time
    int C; ///< candidate pool size at construction time

    // search-time parameters
    int search_L = 16; ///< length of the search path

    int enterpoint; ///< enterpoint

    std::shared_ptr<nsg::Graph<int32_t>> final_graph; ///< NSG graph structure

    bool is_built = false; ///< NSG is built or not

    RandomGenerator rng; ///< random generator

    explicit NSG(int R = 32);

    // build NSG from a KNN graph
    void build(
            Index* storage,
            idx_t n,
            const nsg::Graph<idx_t>& knn_graph,
            bool verbose);

    // reset the graph
    void reset();

    // search interface
    NSGStats search(
            DistanceComputer& dis,
            int k,
            idx_t* I,
            float* D,
            VisitedTable& vt) const;

    // Compute the center point
    void init_graph(Index* storage, const nsg::Graph<idx_t>& knn_graph);

    // Search on a built graph.
    // If collect_fullset is true, the visited nodes will be
    // collected in `fullset`.
    template <bool collect_fullset, class index_t>
    NSGStats search_on_graph(
            const nsg::Graph<index_t>& graph,
            DistanceComputer& dis,
            VisitedTable& vt,
            int ep,
            int pool_size,
            std::vector<Neighbor>& retset,
            std::vector<Node>& fullset) const;

    // Add reverse links
    void add_reverse_links(
            int q,
            std::vector<std::mutex>& locks,
            DistanceComputer& dis,
            nsg::Graph<Node>& graph);

    void sync_prune(
            int q,
            std::vector<Node>& pool,
            DistanceComputer& dis,
            VisitedTable& vt,
            const nsg::Graph<idx_t>& knn_graph,
            nsg::Graph<Node>& graph);

    void link(
            Index* storage,
            const nsg::Graph<idx_t>& knn_graph,
            nsg::Graph<Node>& graph,
            bool verbose);

    // make NSG be fully connected
    int tree_grow(Index* storage, std::vector<int>& degrees);

    // count the size of the connected component
    // using depth first search start by root
    int dfs(VisitedTable& vt, int root, int cnt) const;

    // attach one unlinked node
    int attach_unlinked(
            Index* storage,
            VisitedTable& vt,
            VisitedTable& vt2,
            std::vector<int>& degrees);

    // check the integrity of the NSG built
    void check_graph() const;

    /// Print statistics about the graph connectivity at a given level
    void print_neighbor_stats(int level = 0) const;

    /// Save the degree distribution of the graph to a file
    void save_degree_distribution(const char* filename) const;
};

} // namespace faiss
