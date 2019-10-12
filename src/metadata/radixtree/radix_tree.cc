/*
 *  (c) Copyright 2016-2017 Hewlett Packard Enterprise Development Company LP.
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU Lesser General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *  As an exception, the copyright holders of this Library grant you permission
 *  to (i) compile an Application with the Library, and (ii) distribute the
 *  Application containing code generated by the Library and added to the
 *  Application during this compilation process under terms of your choice,
 *  provided you also meet the terms and conditions of the Application license.
 *
 */

#include <cstring>
#include <list>
#include <string>
#include <stack>
#include <utility> // pair
#include <vector>
#include <iostream>

#include "nvmm/memory_manager.h"
#include "nvmm/heap.h"
#include "nvmm/fam.h"

#include "common.h"
#include "radix_tree.h"
#include "radix_tree_metrics.h"

namespace famradixtree {

constexpr char const *RadixTree::OPEN_BOUNDARY_KEY;

struct RadixTree::Node {
  public:
    char key[RadixTree::MAX_KEY_LEN]; // NOTE: we are actually storing unsigned
                                      // char
    size_t prefix_size;
    Gptr child[256];
    TagGptr value;
};

static inline Gptr cas64(Gptr *target, Gptr old_value, Gptr new_value) {
    return fam_atomic_u64_compare_and_store(
        (uint64_t *)target, (uint64_t)old_value, (uint64_t)new_value);
}

static inline TagGptr casTagGptr(TagGptr *target, TagGptr old_value,
                                 TagGptr new_value) {
    TagGptr result;
    fam_atomic_128_compare_and_store((int64_t *)target, old_value.i64,
                                     new_value.i64, result.i64);
    return result;
}

static inline void loadTagGptr(TagGptr *target, TagGptr &ptr) {
    fam_atomic_128_read((int64_t *)target, ptr.i64);
}

RadixTree::RadixTree(Mmgr *Mmgr, Heap *Heap, RadixTreeMetrics *Metrics,
                     Gptr Root)
    : mmgr(Mmgr), heap(Heap), metrics(Metrics), root(Root) {
    assert(mmgr != NULL);
    assert(heap != NULL);
    if (root == 0) {
        root = heap->Alloc(sizeof(Node));
        assert(root.IsValid());
        Node *root_node = (Node *)toLocal(root);
        assert(root_node);
        root_node->prefix_size = 0;
        for (int i = 0; i < 256; i++) {
            root_node->child[i] = 0;
        }
        root_node->value = TagGptr();
        fam_persist(root_node, sizeof(Node));
    }
}

RadixTree::~RadixTree() {}

//********************************
// Common Helpers                *
//********************************
void *RadixTree::toLocal(const Gptr &gptr) { return mmgr->GlobalToLocal(gptr); }

Gptr RadixTree::get_root() { return root; }

void RadixTree::list(std::function<void(const char *, const size_t, Gptr)> f) {
    Gptr p = root;
    uint64_t level = 0;
    uint64_t depth = 0;
    uint64_t value_cnt = 0;
    uint64_t node_cnt = 0;
    recursive_list(p, f, level, depth, value_cnt, node_cnt);
    printf("\nDepth %lu\n", depth);
    printf("\nValues %lu\n", value_cnt);
    printf("\nNodes %lu\n", node_cnt);
    printf("\nNode size %lu\n", sizeof(Node));
}

void RadixTree::recursive_list(
    Gptr parent, std::function<void(const char *, const size_t, Gptr)> f,
    uint64_t &level, uint64_t &depth, uint64_t &value_cnt, uint64_t &node_cnt) {
    if (parent == 0)
        return;

    Node *n = (Node *)toLocal(parent);
    assert(n);
    fam_invalidate(n, sizeof(Node));

#ifdef DEBUG_VERBOSE
    printf("[%ld: %s (%d)]\n", parent, n->key, n->prefix_size);
    if (n->value.IsValid())
        printf("  * -> %ld\n", n->value.gptr_nomark());
    for (int j = 0; j < 256; j++)
        if (n->child[j] != 0)
            printf("  %c (0x%x) -> %ld\n", j, j, n->child[j]);
#endif
    if (n->value.IsValid()) {
        value_cnt++;
        // printf("%s[%lu] ", std::string(level, ' ').c_str(), level);
        f(n->key, n->prefix_size, n->value.gptr_nomark());
    } else {
        std::string key((char *)n->key, n->prefix_size);
        // printf("%s[%lu] prefix %s\n", std::string(level, ' ').c_str(), level,
        // key.c_str());
    }

    node_cnt++;
    depth = std::max(level, depth);

    level++;
    for (int i = 0; i < 256; i++)
        recursive_list(n->child[i], f, level, depth, value_cnt, node_cnt);
    level--;
}

struct RadixTree::TreeStructure {
    TreeStructure() : depth(0), value_cnt(0), node_cnt(0) {
        nodes_at_level.resize(100);
    }

    void AddNode(int level, Node *n) { nodes_at_level[level].push_back(n); }

    void Report(std::ostream &out) {
        out << "Depth " << depth << std::endl;
        out << "Values " << value_cnt << std::endl;
        out << "Nodes " << node_cnt << std::endl;
        for (int l = 0; l < depth; l++) {
            out << "Level " << l << std::endl;
            out << "\tNodes " << nodes_at_level[l].size() << std::endl;
            int value_cnt = 0;
            for (auto &it : nodes_at_level[l]) {
                Node *n = it;
                if (n->value.IsValid()) {
                    value_cnt++;
                }
            }
            out << "\tValues " << value_cnt << std::endl;
            for (auto &it : nodes_at_level[l]) {
                Node *n = it;
                if (n->value.IsValid()) {
                    std::string key((char *)n->key, n->prefix_size);
                    //                    out << key << " ";
                }
            }
            //            out << std::endl;
        }
    }

    int depth;
    int value_cnt;
    int node_cnt;
    std::vector<std::list<Node *> > nodes_at_level;
};

void RadixTree::structure() {
    Gptr p = root;
    TreeStructure structure;
    recursive_structure(p, 0, structure);
    structure.Report(std::cout);
}

void RadixTree::recursive_structure(Gptr parent, int level,
                                    TreeStructure &structure) {
    if (parent == 0)
        return;

    Node *n = (Node *)toLocal(parent);
    assert(n);
    fam_invalidate(n, sizeof(Node));

    structure.AddNode(level, n);

    structure.node_cnt++;
    structure.depth = std::max(level, structure.depth);

    for (int i = 0; i < 256; i++)
        recursive_structure(n->child[i], level + 1, structure);
}

TagGptr RadixTree::put(const char *key, const size_t key_size, Gptr value,
                       UpdateFlags update) {
    assert(key_size > 0 && key_size <= MAX_KEY_LEN);

    Gptr *p = NULL;
    Gptr q = root;

    Gptr new_leaf_ptr = 0;
    Gptr intermediate_node_ptr = 0;
    Node *intermediate_node = nullptr;
    size_t prefix_size = 0;
    unsigned char existing = 0;
    for (;;) {
        // Find current correct insertion point:
        while (q != 0) {
            Node *n = (Node *)toLocal(q);
            assert(n);
            size_t i, max_i = std::min(key_size, n->prefix_size);
            for (i = 0; i < max_i; i++)
                if (key[i] != n->key[i])
                    break;

#ifdef PMEM
            fam_invalidate(&n->child, sizeof(n->child) + sizeof(n->value));
#endif
            if (i < n->prefix_size) {
                // does not match the entire prefix, we have to do a split
                prefix_size = i;
                existing = (unsigned char)n->key[i];
                break;
                // will always go to case 2
            } else {
                // the key so far has matched the entire prefix
                if (!update)
                    assert(i == n->prefix_size);

                // assert(key_size >= n->prefix_size);
                // assert(i==n->prefix_size);
                if (key_size == i) {
                    // match the entire prefix
                    if (intermediate_node_ptr)
                        heap->Free(intermediate_node_ptr);
                    if (new_leaf_ptr)
                        heap->Free(new_leaf_ptr);

                    TagGptr *tp = &n->value;
                    TagGptr tq;
#ifdef PMEM
                    tq = *tp;
#else
                    loadTagGptr(tp, tq);
#endif
                    /* When there is a key match, if update is set, that means
                     * It will update the vaule associated with the key with the
                     * new value given.
                     */
                    if (update) {
                        for (;;) {
                            TagGptr seen_tq = casTagGptr(
                                tp, tq, TagGptr(value, tq.tag() + 1));
                            if (seen_tq == tq) {
                                return tq;
                            }
                            tq = seen_tq;
                        }
                    } else {
                        /* In this case update is not set, that means it found
                         * a value for the key and the same value is returned.
                         */
                        if (tq.IsValid()) {
                            return tq;
                        }
                        /* We have a match for the key, but value is not set. Update the 
                         * value for the node and return.
                         */
                        TagGptr seen_tq = casTagGptr(tp, tq, TagGptr(value, tq.tag() + 1));
                        if (seen_tq == tq) {
                            return tq;
                        }
                        break;
                    }
                } else {
                    // the key is longer
                    p = &n->child[(unsigned char)key[i]];
#ifdef PMEM
                    q = *p;
#else
                    q = fam_atomic_u64_read((uint64_t *)p);
#endif
                    // will always go to case 1 if q==0
                }
            }
        }

        // case 1:
        // no split but need to insert a new leaf node:
        if (q == 0) {
            if (new_leaf_ptr == 0) {
                int cnt = alloc_retry_cnt;
                while (new_leaf_ptr == 0 && (cnt--) > 0)
                    new_leaf_ptr = heap->Alloc(sizeof(Node));
                assert(new_leaf_ptr.IsValid());
                Node *new_leaf = (Node *)toLocal(new_leaf_ptr);
                assert(new_leaf);
                memcpy(new_leaf->key, key, key_size);
                new_leaf->prefix_size = key_size;
                new_leaf->value = TagGptr(value, 0);
                fam_persist(new_leaf, sizeof(Node));
            }

            Gptr seen_q = cas64(p, q, new_leaf_ptr);
            if (seen_q == q) {
                if (intermediate_node_ptr)
                    heap->Free(intermediate_node_ptr);
                return TagGptr();
            }
            q = seen_q;
            continue;
        }

        // case 2:
        // split
        if (intermediate_node_ptr == 0) {
            int cnt = alloc_retry_cnt;
            while (intermediate_node_ptr == 0 && (cnt--) > 0)
                intermediate_node_ptr = heap->Alloc(sizeof(Node));
            assert(intermediate_node_ptr.IsValid());
            intermediate_node = (Node *)toLocal(intermediate_node_ptr);
            assert(intermediate_node);
            // we don't just copy the current common prefix because the prefix
            // at this node may
            // change when the final pointer swing fails
            // so it is easier to just copy the entire key and update the
            // prefix_size later
            memcpy(intermediate_node->key, key, key_size);
            for (int i = 0; i < 256; i++)
                intermediate_node->child[i] = 0;
        }

        // where do we store this value?
        if (prefix_size == key_size) {
            // no extra node needed
            intermediate_node->value = TagGptr(value, 0);

            // link q
            intermediate_node->prefix_size = prefix_size;
            intermediate_node->child[existing] = q;
            fam_persist(intermediate_node, sizeof(Node));

            Gptr seen_q = cas64(p, q, intermediate_node_ptr);
            if (seen_q == q) {
                if (new_leaf_ptr)
                    heap->Free(new_leaf_ptr);
                return TagGptr();
            }

            q = seen_q;
        } else {
            // need a new leaf node
            if (new_leaf_ptr == 0) {
                int cnt = alloc_retry_cnt;
                while (new_leaf_ptr == 0 && (cnt--) > 0)
                    new_leaf_ptr = heap->Alloc(sizeof(Node));
                assert(new_leaf_ptr.IsValid());
                Node *new_leaf = (Node *)toLocal(new_leaf_ptr);
                assert(new_leaf);
                memcpy(new_leaf->key, key, key_size);
                new_leaf->prefix_size = key_size;
                new_leaf->value = TagGptr(value, 0);
                fam_persist(new_leaf, sizeof(Node));
            }
            intermediate_node->child[(unsigned char)key[prefix_size]] =
                new_leaf_ptr;

            // link q
            intermediate_node->prefix_size = prefix_size;
            intermediate_node->child[existing] = q;
            fam_persist(intermediate_node, sizeof(Node));

            Gptr seen_q = cas64(p, q, intermediate_node_ptr);
            if (seen_q == q)
                return TagGptr();

            q = seen_q;
        }
    }
}

TagGptr RadixTree::get(const char *key, const size_t key_size) {
    assert(key_size > 0 && key_size <= MAX_KEY_LEN);
    Gptr *p = NULL;
    Gptr q = root;

    int pointer_traversals = 0;

    while (q != 0) {
        Node *n = (Node *)toLocal(q);
        assert(n);

        int result =
            fam_memcmp(key, n->key, std::min(n->prefix_size, key_size));
        if (result != 0)
            return TagGptr();

#ifdef PMEM
        fam_invalidate(&n->child, sizeof(n->child) + sizeof(n->value));
#endif

        if (n->prefix_size == key_size) {
            TagGptr *tp = &n->value;
            TagGptr tq;
#ifdef PMEM
            tq = *tp;
#else
            loadTagGptr(tp, tq);
#endif
            METRIC_HISTOGRAM_UPDATE(metrics, pointer_traversal_,
                                    pointer_traversals);

            return tq;
        }

        // assert(n->prefix_size<key_size);
        p = &n->child[(unsigned char)key[n->prefix_size]];

#ifdef PMEM
        q = *p;
#else
        q = fam_atomic_u64_read((uint64_t *)p);
#endif

        pointer_traversals++;
    }

    return TagGptr();
}

TagGptr RadixTree::destroy(const char *key, const size_t key_size) {
    assert(key_size > 0 && key_size <= MAX_KEY_LEN);
    Gptr *p = NULL;
    Gptr q = root;

    while (q != 0) {
        Node *n = (Node *)toLocal(q);
        assert(n);

        int result =
            fam_memcmp(key, n->key, std::min(n->prefix_size, key_size));
        if (result != 0)
            return TagGptr();

#ifdef PMEM
        fam_invalidate(&n->child, sizeof(n->child) + sizeof(n->value));
#endif
        if (n->prefix_size == key_size) {
            TagGptr *tp = &n->value;
            TagGptr tq;
#ifdef PMEM
            tq = *tp;
#else
            loadTagGptr(tp, tq);
#endif
            for (;;) {
                TagGptr seen_tq = casTagGptr(tp, tq, TagGptr(0, tq.tag() + 1));
                if (seen_tq == tq) {
                    return tq;
                }
                tq = seen_tq;
            }
        }

        p = &n->child[(unsigned char)key[n->prefix_size]];
#ifdef PMEM
        q = *p;
#else
        q = fam_atomic_u64_read((uint64_t *)p);
#endif
    }

    return TagGptr();
}

// find the next key within the requested range
// find the next key that is less than (or equal to, if end_key_inclusive==true)
// the end key
// returns true when a valid key is found
bool RadixTree::next_value(Iter &iter) {
    char const *key = iter.end_key.data();
    size_t key_size = iter.end_key.size();

    // std::cout << "next_value: end_key " << key_size << std::endl;
    // for(int i=0; i<key_size; i++) {
    //     std::cout << (uint64_t)key[i] << " ";
    // }
    // std::cout << std::endl;

    Gptr *p, q;
    while (iter.node != 0) {
        while (iter.next_pos == 257) {
            if (iter.path.empty())
                return false;
            auto parent = iter.path.top();
            iter.path.pop();
            iter.node = parent.first;
            iter.next_pos = parent.second + 1 + 1;
            // std::cout << "next_value going up at " << iter.next_pos-1 <<
            // std::endl;
        }

        Node *n = (Node *)toLocal(iter.node);
        assert(n);

        // std::cout << "next_value: current node key " << n->prefix_size <<
        // std::endl;
        // for(int i=0; i<n->prefix_size; i++) {
        //     std::cout << (uint64_t)n->key[i] << " ";
        // }
        // std::cout << std::endl;

        // TODO: cache comparison result in iter?
        int result;
        if (iter.end_key_open)
            result = 1;
        else
            result =
                fam_memcmp(key, n->key, std::min(n->prefix_size, key_size));
        if (result < 0) {
            // std::cout << "next_value result < 0 " << std::endl;
            return false;
        } else if (result > 0) {
// std::cout << "next_value result > 0 " << std::endl;
// every key in this subtree is valid
// so we go through all the child pointers

// std::cout << "next_value !!! next_pos " << iter.next_pos << std::endl;

#ifdef PMEM
            fam_invalidate(&n->child, sizeof(n->child) + sizeof(n->value));
#endif

            // special case: check the value ptr
            if (iter.next_pos == 0) {
                // std::cout << "next_value checking value " << std::endl;
                iter.next_pos++;

                TagGptr *tp = &n->value;
                TagGptr tq;
#ifdef PMEM
                tq = *tp;
#else
                loadTagGptr(tp, tq);
#endif
                if (tq.IsValid()) {
                    iter.key = std::string((char *)&n->key, n->prefix_size);
                    iter.value = tq;
                    return true;
                }
            }

            // check the next child ptr
            for (; iter.next_pos <= 256; iter.next_pos++) {
                // std::cout << "next_value checking ptr at " << iter.next_pos-1
                // << std::endl;
                p = &n->child[iter.next_pos - 1];
#ifdef PMEM
                q = *p;
#else
                q = fam_atomic_u64_read((uint64_t *)p);
#endif
                if (q) {
                    // std::cout << "next_value going down at " <<
                    // iter.next_pos-1 << std::endl;
                    iter.path.push(
                        std::make_pair(iter.node, iter.next_pos - 1));
                    iter.node = q;
                    iter.next_pos = 0;
                    break;
                }
            }

            // then we go up
        } else {
            // std::cout << "next_value result == 0 " << std::endl;
            // assert(result == 0);
            if (n->prefix_size == key_size) {
                iter.node = 0; // indicating there is no more valid keys
                if (iter.end_key_inclusive) {
                    // node->value would be the last valid key
                    // so we just check the value ptr
                    TagGptr *tp = &n->value;
                    TagGptr tq;
#ifdef PMEM
                    tq = *tp;
#else
                    loadTagGptr(tp, tq);
#endif

                    if (iter.next_pos == 0 && tq.IsValid()) {

                        iter.key = std::string((char *)&n->key, n->prefix_size);
                        iter.value = tq;
                        return true;
                    }
                    return false;
                } else {
                    return false;
                }
            } else {
                // assert(n->prefix_size < key_size);
                // we check all child pointers up to key[n->prefix_size]

                // special case: check the value ptr
                if (iter.next_pos == 0) {
                    iter.next_pos++;
                    TagGptr *tp = &n->value;
                    TagGptr tq;
#ifdef PMEM
                    tq = *tp;
#else
                    loadTagGptr(tp, tq);
#endif
                    if (tq.IsValid()) {
                        iter.key = std::string((char *)&n->key, n->prefix_size);
                        iter.value = tq;
                        return true;
                    }
                }
                // check the next child ptr
                uint64_t upper_bound = (uint64_t)key[n->prefix_size];
                for (; iter.next_pos <= upper_bound + 1; iter.next_pos++) {
                    // std::cout << "next_value checking ptr at " <<
                    // iter.next_pos-1 << std::endl;
                    p = &n->child[iter.next_pos - 1];
#ifdef PMEM
                    q = *p;
#else
                    q = fam_atomic_u64_read((uint64_t *)p);
#endif
                    if (q) {
                        // std::cout << "next_value going down at " <<
                        // iter.next_pos-1 << std::endl;
                        iter.path.push(
                            std::make_pair(iter.node, iter.next_pos - 1));
                        iter.node = q;
                        iter.next_pos = 0;
                        break;
                    }
                }

                if (iter.next_pos > upper_bound + 1) {
                    // then we are done!
                    iter.node = 0;
                    return false;
                }
            }
        }
    }

    return false;
}

// find the starting point for next_value() to get the first key within the
// requested range
// find the first potential key that is greater than (or equal to, if
// begin_key_inclusive==true) the begin key
bool RadixTree::lower_bound(Iter &iter) {
    iter.node = root;
    iter.next_pos = 0;
    assert(iter.key.empty());
    iter.value = TagGptr();

    char const *key = iter.begin_key.data();
    size_t key_size = iter.begin_key.size();
    // std::cout << "lower_bound: begin_key " << key_size << std::endl;
    // for(int i=0; i<key_size; i++) {
    //     std::cout << (uint64_t)key[i] << " ";
    // }
    // std::cout << std::endl;

    while (iter.node != 0) {
        Node *n = (Node *)toLocal(iter.node);
        assert(n);

        // std::cout << "lower_bound !!! current node key " << n->prefix_size <<
        // std::endl;
        // for(int i=0; i<n->prefix_size; i++) {
        //     std::cout << (uint64_t)n->key[i] << " ";
        // }
        // std::cout << std::endl;

        int result;
        if (iter.begin_key_open)
            result = -1;
        else
            result =
                fam_memcmp(key, n->key, std::min(n->prefix_size, key_size));

        if (result > 0) {
            // std::cout << "lower_bound !!! result > 0 " << std::endl;
            // oops, begin key > n->key
            // we have to go up and the next node is our starting point
            iter.next_pos =
                257; // indicating we are done with this node and want to go up
            return next_value(iter);
        } else if (result < 0) {
            // std::cout << "lower_bound !!! result < 0 " << std::endl;
            // begin key < n->key
            // current node is our starting point
            // assert(iter.next_pos == 0);
            return next_value(iter);
        } else {
            // std::cout << "lower_bound !!! result == 0 " << std::endl;
            // assert(result == 0);
            if (n->prefix_size == key_size) {
                // begin key == n->key
                if (iter.begin_key_inclusive) {
                    // current node is our starting point
                    // assert(iter.next_pos == 0);
                    // std::cout << "lower_bound now !!!" << std::endl;
                    return next_value(iter);
                } else {
                    // the first child is our starting point
                    iter.next_pos = 1;
                    return next_value(iter);
                }
            } else {
                // assert(n->prefix_size < key_size);
                unsigned char idx = (unsigned char)key[n->prefix_size];
#ifdef PMEM
                fam_invalidate(&n->child, sizeof(n->child) + sizeof(n->value));
#endif
                Gptr *p, q;
                p = &n->child[(unsigned char)idx];
#ifdef PMEM
                q = *p;
#else
                q = fam_atomic_u64_read((uint64_t *)p);
#endif
                if (q) {
                    // we have not yet found the starting point
                    // keep going down
                    // std::cout << "lower_bound !!! going down at " <<
                    // (uint64_t)idx << std::endl;
                    iter.path.push(std::make_pair(iter.node, idx));
                    iter.node = q;
                    continue;
                } else {
                    // the next node is our starting point
                    // std::cout << "lower_bound !!! next node " <<
                    // (uint64_t)idx << std::endl;
                    iter.next_pos = idx + 1;
                    return next_value(iter);
                }
            }
        }
    }
    iter.node = 0;
    return false;
}

int RadixTree::scan(Iter &iter, char *key, size_t &key_size, TagGptr &val,
                    const char *begin_key, const size_t begin_key_size,
                    const bool begin_key_inclusive, const char *end_key,
                    const size_t end_key_size, const bool end_key_inclusive) {
    assert(begin_key_size > 0 && begin_key_size <= MAX_KEY_LEN);
    assert(end_key_size > 0 && end_key_size <= MAX_KEY_LEN);

    iter.node = 0;
    iter.next_pos = 0;
    iter.key.clear();
    iter.value = TagGptr();
    {
        std::stack<std::pair<Gptr, uint64_t> > tmp;
        tmp.swap(iter.path);
    }

    // std::cout << ">>> scan " << std::endl;
    static const std::string OPEN_BOUNDARY_STR =
        std::string((char const *)OPEN_BOUNDARY_KEY, OPEN_BOUNDARY_KEY_SIZE);
    iter.begin_key = std::string(begin_key, begin_key_size);
    iter.begin_key_inclusive = begin_key_inclusive;

    if (iter.begin_key == OPEN_BOUNDARY_STR &&
        iter.end_key_inclusive == false) {
        // a valid open begin key
        iter.begin_key_open = true;
        // std::cout << " open begin key" << std::endl;
    } else {
        iter.begin_key_open = false;
    }

    iter.end_key = std::string(end_key, end_key_size);
    iter.end_key_inclusive = end_key_inclusive;

    if (iter.end_key == OPEN_BOUNDARY_STR && iter.end_key_inclusive == false) {
        // a valid open end key
        iter.end_key_open = true;
        // std::cout << " open end key" << std::endl;
    } else {
        iter.end_key_open = false;
    }

    // point query
    if (iter.begin_key == iter.end_key && begin_key_inclusive &&
        end_key_inclusive) {
        val = get(begin_key, begin_key_size);
        if (val.IsValid()) {
            memcpy(key, begin_key, begin_key_size);
            key_size = begin_key_size;
            return 0;
        } else
            return -1; // key not found
    }

    // range query
    if ((iter.begin_key_open || iter.end_key_open) ||
        (iter.begin_key < iter.end_key)) {
        if (lower_bound(iter)) {
            val = iter.value;
            key_size = (int)iter.key.size();
            memcpy(key, iter.key.data(), key_size);
            return 0;
        }
    }

    return -1; // key not found
}

int RadixTree::get_next(Iter &iter, char *key, size_t &key_size, TagGptr &val) {
    // std::cout << ">>> get_next " << std::endl;
    if (next_value(iter)) {
        val = iter.value;
        key_size = (int)iter.key.size();
        memcpy(key, iter.key.data(), key_size);
        return 0;
    }
    return -1; // key not found
}

/*
  for consistent DRAM caching
*/
std::pair<Gptr, TagGptr> RadixTree::putC(const char *key, const size_t key_size,
                                         Gptr value, TagGptr &old_value) {
    assert(key_size > 0 && key_size <= MAX_KEY_LEN);

    Gptr *p = NULL;
    Gptr q = root;

    Gptr new_leaf_ptr = 0;
    Gptr intermediate_node_ptr = 0;
    Node *intermediate_node = nullptr;
    size_t prefix_size = 0;
    unsigned char existing = 0;
    for (;;) {
        // Find current correct insertion point:
        while (q != 0) {
            Node *n = (Node *)toLocal(q);
            assert(n);
            size_t i, maxi = std::min(key_size, n->prefix_size);
            for (i = 0; i < maxi; i++)
                if (key[i] != n->key[i])
                    break;

#ifdef PMEM
            fam_invalidate(&n->child, sizeof(n->child) + sizeof(n->value));
#endif
            if (i < n->prefix_size) {
                // does not match the entire prefix, we have to do a split
                prefix_size = i;
                existing = (unsigned char)n->key[i];
                break;
                // will always go to case 2
            } else {
                // the key so far has matched the entire prefix
                // assert(key_size >= n->prefix_size);
                // assert(i==n->prefix_size);
                if (key_size == i) {
                    // UPDATE
                    // match the entire prefix
                    if (intermediate_node_ptr)
                        heap->Free(intermediate_node_ptr);
                    if (new_leaf_ptr)
                        heap->Free(new_leaf_ptr);

                    TagGptr *tp = &n->value;
                    TagGptr tq;
#ifdef PMEM
                    tq = *tp;
#else
                    loadTagGptr(tp, tq);
#endif
                    for (;;) {
                        TagGptr new_value = TagGptr(value, tq.tag() + 1);
                        TagGptr seen_tq = casTagGptr(tp, tq, new_value);
                        if (seen_tq == tq) {
                            old_value = tq;
                            return std::make_pair(q, new_value);
                        }
                        tq = seen_tq;
                    }
                } else {
                    // the key is longer
                    p = &n->child[(unsigned char)key[i]];
#ifdef PMEM
                    q = *p;
#else
                    q = fam_atomic_u64_read((uint64_t *)p);
#endif
                    // will always go to case 1 if q==0
                }
            }
        }

        // INSERT
        // case 1:
        // no split but need to insert a new leaf node:
        if (q == 0) {
            if (new_leaf_ptr == 0) {
                int cnt = alloc_retry_cnt;
                while (new_leaf_ptr == 0 && (cnt--) > 0)
                    new_leaf_ptr = heap->Alloc(sizeof(Node));
                assert(new_leaf_ptr.IsValid());
                Node *new_leaf = (Node *)toLocal(new_leaf_ptr);
                assert(new_leaf);
                memcpy(new_leaf->key, key, key_size);
                new_leaf->prefix_size = key_size;
                new_leaf->value = TagGptr(value, 0);
                fam_persist(new_leaf, sizeof(Node));
            }

            Gptr seen_q = cas64(p, q, new_leaf_ptr);
            if (seen_q == q) {
                if (intermediate_node_ptr)
                    heap->Free(intermediate_node_ptr);
                old_value = TagGptr();
                TagGptr new_value = TagGptr(value, 0);
                return std::make_pair(new_leaf_ptr, new_value);
            }
            q = seen_q;
            continue;
        }

        // case 2:
        // split
        if (intermediate_node_ptr == 0) {
            int cnt = alloc_retry_cnt;
            while (intermediate_node_ptr == 0 && (cnt--) > 0)
                intermediate_node_ptr = heap->Alloc(sizeof(Node));
            assert(intermediate_node_ptr.IsValid());
            intermediate_node = (Node *)toLocal(intermediate_node_ptr);
            assert(intermediate_node);
            // we don't just copy the current common prefix because the prefix
            // at this node may
            // change when the final pointer swing fails
            // so it is easier to just copy the entire key and update the
            // prefix_size later
            memcpy(intermediate_node->key, key, key_size);
            for (int i = 0; i < 256; i++)
                intermediate_node->child[i] = 0;
        }

        // where do we store this value?
        if (prefix_size == key_size) {
            // no extra node needed
            intermediate_node->value = TagGptr(value, 0);

            // link q
            intermediate_node->prefix_size = prefix_size;
            intermediate_node->child[existing] = q;
            fam_persist(intermediate_node, sizeof(Node));

            Gptr seen_q = cas64(p, q, intermediate_node_ptr);
            if (seen_q == q) {
                if (new_leaf_ptr)
                    heap->Free(new_leaf_ptr);
                old_value = TagGptr();
                TagGptr new_value = TagGptr(value, 0);
                return std::make_pair(intermediate_node_ptr, new_value);
            }
            q = seen_q;
        } else {
            // need a new leaf node
            if (new_leaf_ptr == 0) {
                int cnt = alloc_retry_cnt;
                while (new_leaf_ptr == 0 && (cnt--) > 0)
                    new_leaf_ptr = heap->Alloc(sizeof(Node));
                assert(new_leaf_ptr.IsValid());
                Node *new_leaf = (Node *)toLocal(new_leaf_ptr);
                assert(new_leaf);
                memcpy(new_leaf->key, key, key_size);
                new_leaf->prefix_size = key_size;
                new_leaf->value = TagGptr(value, 0);
                fam_persist(new_leaf, sizeof(Node));
            }
            intermediate_node->child[(unsigned char)key[prefix_size]] =
                new_leaf_ptr;

            // link q
            intermediate_node->prefix_size = prefix_size;
            intermediate_node->child[existing] = q;
            fam_persist(intermediate_node, sizeof(Node));

            Gptr seen_q = cas64(p, q, intermediate_node_ptr);
            if (seen_q == q) {
                old_value = TagGptr();
                TagGptr new_value = TagGptr(value, 0);
                return std::make_pair(new_leaf_ptr, new_value);
            }
            q = seen_q;
        }
    }
}

TagGptr RadixTree::putC(Gptr const key_ptr, Gptr value, TagGptr &old_value) {
    Gptr q = key_ptr;
    assert(q != 0);
    Node *n = (Node *)toLocal(q);
    assert(n);

#ifdef PMEM
    fam_invalidate(&n->value, sizeof(n->value));
#endif

    TagGptr *tp = &n->value;
    TagGptr tq;
#ifdef PMEM
    tq = *tp;
#else
    loadTagGptr(tp, tq);
#endif
    for (;;) {
        TagGptr new_value = TagGptr(value, tq.tag() + 1);
        TagGptr seen_tq = casTagGptr(tp, tq, new_value);
        if (seen_tq == tq) {
            old_value = tq;
            return new_value;
        }
        tq = seen_tq;
    }
}

std::pair<Gptr, TagGptr> RadixTree::getC(const char *key,
                                         const size_t key_size) {
    assert(key_size > 0 && key_size <= MAX_KEY_LEN);
    Gptr *p = NULL;
    Gptr q = root;

    while (q != 0) {
        Node *n = (Node *)toLocal(q);
        assert(n);

        int result =
            fam_memcmp(key, n->key, std::min(n->prefix_size, key_size));
        if (result != 0)
            return std::make_pair(Gptr(), TagGptr());

#ifdef PMEM
        fam_invalidate(&n->child, sizeof(n->child) + sizeof(n->value));
#endif

        if (n->prefix_size == key_size) {
            TagGptr *tp = &n->value;
            TagGptr tq;
#ifdef PMEM
            tq = *tp;
#else
            loadTagGptr(tp, tq);
#endif
            return std::make_pair(q, tq);
        }

        // assert(n->prefix_size<key_size);
        p = &n->child[(unsigned char)key[n->prefix_size]];
#ifdef PMEM
        q = *p;
#else
        q = fam_atomic_u64_read((uint64_t *)p);
#endif
    }

    return std::make_pair(Gptr(), TagGptr());
}

TagGptr RadixTree::getC(Gptr const key_ptr) {
    Gptr q = key_ptr;
    assert(q != 0);
    Node *n = (Node *)toLocal(q);
    assert(n);

#ifdef PMEM
    fam_invalidate(&n->value, sizeof(n->value));
#endif

    TagGptr *tp = &n->value;
    TagGptr tq;
#ifdef PMEM
    tq = *tp;
#else
    loadTagGptr(tp, tq);
#endif
    return tq;
}

std::pair<Gptr, TagGptr> RadixTree::destroyC(const char *key,
                                             const size_t key_size,
                                             TagGptr &old_value) {
    assert(key_size > 0 && key_size <= MAX_KEY_LEN);
    Gptr *p = NULL;
    Gptr q = root;

    while (q != 0) {
        Node *n = (Node *)toLocal(q);
        assert(n);

        int result =
            fam_memcmp(key, n->key, std::min(n->prefix_size, key_size));
        if (result != 0)
            return std::make_pair(Gptr(), TagGptr());

#ifdef PMEM
        fam_invalidate(&n->child, sizeof(n->child) + sizeof(n->value));
#endif
        if (n->prefix_size == key_size) {
            TagGptr *tp = &n->value;
            TagGptr tq;
#ifdef PMEM
            tq = *tp;
#else
            loadTagGptr(tp, tq);
#endif
            for (;;) {
                TagGptr new_value = TagGptr(0, tq.tag() + 1);
                TagGptr seen_tq = casTagGptr(tp, tq, new_value);
                if (seen_tq == tq) {
                    old_value = tq;
                    return std::make_pair(q, new_value);
                }
                tq = seen_tq;
            }
        }

        p = &n->child[(unsigned char)key[n->prefix_size]];
#ifdef PMEM
        q = *p;
#else
        q = fam_atomic_u64_read((uint64_t *)p);
#endif
    }

    return std::make_pair(Gptr(), TagGptr());
}

TagGptr RadixTree::destroyC(Gptr const key_ptr, TagGptr &old_value) {
    Gptr q = key_ptr;
    assert(q != 0);
    Node *n = (Node *)toLocal(q);
    assert(n);

#ifdef PMEM
    fam_invalidate(&n->value, sizeof(n->value));
#endif

    TagGptr *tp = &n->value;
    TagGptr tq;
#ifdef PMEM
    tq = *tp;
#else
    loadTagGptr(tp, tq);
#endif
    for (;;) {
        TagGptr new_value = TagGptr(0, tq.tag() + 1);
        TagGptr seen_tq = casTagGptr(tp, tq, new_value);
        if (seen_tq == tq) {
            old_value = tq;
            return new_value;
        }
        tq = seen_tq;
    }
}

} // end namespace bold
