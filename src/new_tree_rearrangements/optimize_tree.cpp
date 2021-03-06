#include "src/new_tree_rearrangements/mutation_annotated_tree.hpp"
#include "tree_rearrangement_internal.hpp"
#include "priority_conflict_resolver.hpp"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <tbb/blocked_range.h>
#include <tbb/concurrent_vector.h>
#include <tbb/parallel_for.h>
#include <tbb/partitioner.h>
#include <tbb/task.h>
#include <thread>
#include <unistd.h>
#include "Profitable_Moves_Enumerators/Profitable_Moves_Enumerators.hpp"
void find_profitable_moves(MAT::Node *src, output_t &out,int radius,
    stack_allocator<Mutation_Count_Change>& allocator
#ifdef DEBUG_PARSIMONY_SCORE_CHANGE_CORRECT
,MAT::Tree* tree
#endif
);
thread_local stack_allocator<Mutation_Count_Change> FIFO_allocator;
extern tbb::task_group_context search_context;
MAT::Node* get_LCA(MAT::Node* src,MAT::Node* dst){
    while (src!=dst) {
        //as dfs index of parent node will always smaller than its children's , so 
        //substitute the node with larger dfs index with its parent and see whether 
        //it will be the same as the other node
        if (src->dfs_index>dst->dfs_index) {
            src=src->parent;
        }
        else if (src->dfs_index<dst->dfs_index) {
            dst=dst->parent;
        }
    }
    return src;
}

bool check_not_ancestor(MAT::Node* dst,MAT::Node* src){
    while (dst) {
        if (dst==src) {
            return false;
        }
        dst=dst->parent;
    }
    return true;
}
//The progress bar that does a linear extrapolation of time left for a round
static void print_progress(
    const std::atomic<int> *checked_nodes,
    std::chrono::time_point<
        std::chrono::steady_clock,
        std::chrono::duration<long, struct std::ratio<1, 1000000000>>>
        start_time,
    size_t total_nodes,
    const tbb::concurrent_vector<MAT::Node *> *deferred_nodes,
    bool *done,std::mutex* done_mutex,size_t max_queued_moves) {
    while (true) {
        {
            if (*done) {
                return;
            }
            std::unique_lock<std::mutex> done_lock(*done_mutex);
            //I want it to print every second, but it somehow managed to print every minute...
            if (timed_print_progress) {
                progress_bar_cv.wait_for(done_lock,std::chrono::seconds(1));
            }else {
                progress_bar_cv.wait(done_lock);
            }
        }
        if (deferred_nodes->size()>max_queued_moves||*done) {
            return;
        }
        int checked_nodes_temp = checked_nodes->load(std::memory_order_relaxed);
        std::chrono::duration<double> elpased_time =
            std::chrono::steady_clock::now() - start_time;
        double seconds_left = elpased_time.count() *
                              (total_nodes - checked_nodes_temp) /
                              checked_nodes_temp;
        fprintf(stderr,"\rchecked %d nodes, estimate %f min left,found %zu nodes "
               "profitable",
               checked_nodes_temp, seconds_left / 60, deferred_nodes->size());
        if (seconds_left < 60) {
            break;
        }
    }
}
size_t optimize_tree(std::vector<MAT::Node *> &bfs_ordered_nodes,
              tbb::concurrent_vector<MAT::Node *> &nodes_to_search,
              MAT::Tree &t,int radius,FILE* log,size_t max_queued_moves
              #ifndef NDEBUG
              , Original_State_t origin_states
            #endif
              ) {
    fprintf(stderr, "%zu nodes to search \n", nodes_to_search.size());
    fprintf(stderr, "Node size: %zu\n", bfs_ordered_nodes.size());
    for(auto node:bfs_ordered_nodes){
        node->changed=false;
    }
    //for resolving conflicting moves
    tbb::concurrent_vector<MAT::Node *> deferred_nodes;
    Deferred_Move_t deferred_moves;
    Conflict_Resolver resolver(bfs_ordered_nodes.size(),deferred_moves,log);
    //progress bar
    std::atomic<int> checked_nodes(0);
    bool done=false;
    std::mutex done_mutex;
    auto search_start= std::chrono::steady_clock::now();
    std::thread progress_meter(print_progress,&checked_nodes,search_start, nodes_to_search.size(), &deferred_nodes,&done,&done_mutex,max_queued_moves);
    fputs("Start searching for profitable moves\n",stderr);
    //Actual search of profitable moves
    output_t out;
    tbb::parallel_for(tbb::blocked_range<size_t>(0, nodes_to_search.size()),
                      [&nodes_to_search, &resolver,
                       &deferred_nodes,radius,&checked_nodes,max_queued_moves
                              #ifdef DEBUG_PARSIMONY_SCORE_CHANGE_CORRECT
,&t
#endif
                       ](tbb::blocked_range<size_t> r) {
                        stack_allocator<Mutation_Count_Change>& this_thread_FIFO_allocator=FIFO_allocator;
                          for (size_t i = r.begin(); i < r.end(); i++) {
                              if (search_context.is_group_execution_cancelled()) {
                                  break;
                              }
                              if(deferred_nodes.size()<max_queued_moves){
                              output_t out;
                              find_profitable_moves(nodes_to_search[i], out, radius,this_thread_FIFO_allocator
                              #ifdef DEBUG_PARSIMONY_SCORE_CHANGE_CORRECT
,&t
#endif
                              );
                               assert(this_thread_FIFO_allocator.empty());
                              if (!out.moves.empty()) {
                                  //resolve conflicts
                                  deferred_nodes.push_back(
                                      out.moves[0]->get_src());
                                  resolver(out.moves);
                              }
                              checked_nodes.fetch_add(1,std::memory_order_relaxed);
                              }else{
                                  deferred_nodes.push_back(nodes_to_search[i]);
                              }
                          }
                      },search_context);
    {
        //stop the progress bar
        //std::unique_lock<std::mutex> done_lock(done_mutex);
        done=true;
        //done_lock.unlock();
        progress_bar_cv.notify_all();
    }
    auto searh_end=std::chrono::steady_clock::now();
    std::chrono::duration<double> elpased_time =searh_end-search_start;
    fprintf(stderr, "Search took %f minutes\n",elpased_time.count()/60);
    //apply moves
    fputs("Start applying moves\n",stderr);
    std::vector<Profitable_Moves_ptr_t> all_moves;
    resolver.schedule_moves(all_moves);
    apply_moves(all_moves, t, bfs_ordered_nodes, deferred_nodes
#ifdef CHECK_STATE_REASSIGN
    ,origin_states
#endif
    );
    auto apply_end=std::chrono::steady_clock::now();
    elpased_time =apply_end-searh_end;
    fprintf(stderr, "apply moves took %f s\n",elpased_time.count());
    //recycle conflicting moves
    fputs("Start recycling conflicting moves\n",stderr);
    while (!deferred_moves.empty()) {
        bfs_ordered_nodes=t.breadth_first_expansion();
        {Deferred_Move_t deferred_moves_next;
        static FILE* ignored=fopen("/dev/null", "w");
        Conflict_Resolver resolver(bfs_ordered_nodes.size(),deferred_moves_next,ignored);
        if (timed_print_progress) {
            fprintf(stderr,"\rrecycling conflicting moves, %zu left",deferred_moves.size());
        }
        tbb::parallel_for(tbb::blocked_range<size_t>(0,deferred_moves.size()),[&deferred_moves,&resolver,&t](const tbb::blocked_range<size_t>& r){
            for (size_t i=r.begin(); i<r.end(); i++) {
                MAT::Node* src=t.get_node(deferred_moves[i].first);
                MAT::Node* dst=t.get_node(deferred_moves[i].second);
                if (src&&dst) {
                    output_t out;
                        if (check_not_ancestor(dst, src)) {
                            individual_move(src,dst,get_LCA(src, dst),out
                              #ifdef DEBUG_PARSIMONY_SCORE_CHANGE_CORRECT
,&t
#endif
                            );
                        }
                    if (!out.moves.empty()) {
                        resolver(out.moves);
                    }
                }
                
            }
        });
        all_moves.clear();
        resolver.schedule_moves(all_moves);
    apply_moves(all_moves, t, bfs_ordered_nodes, deferred_nodes
#ifdef CHECK_STATE_REASSIGN
    ,origin_states
#endif
    );
        deferred_moves=std::move(deferred_moves_next);}
    
    }
    auto recycle_end=std::chrono::steady_clock::now();
    elpased_time =recycle_end-apply_end;
    #ifndef NDEBUG
    check_samples(t.root, origin_states, &t);
    #endif
    nodes_to_search = std::move(deferred_nodes);
    progress_meter.join();
    fprintf(stderr, "recycling moves took %f s\n",elpased_time.count());
    return t.get_parsimony_score();
}
std::unordered_map<MAT::Mutation,
                   std::unordered_map<std::string, nuc_one_hot> *,
                   Mutation_Pos_Only_Hash, Mutation_Pos_Only_Comparator>
    mutated_positions;
void save_final_tree(MAT::Tree &t, Original_State_t& origin_states,
                            const std::string &output_path) {
#ifndef NDEBUG
    check_samples(t.root, origin_states, &t);
#endif
    std::vector<MAT::Node *> dfs = t.depth_first_expansion();
    tbb::parallel_for(tbb::blocked_range<size_t>(0, dfs.size()),
                      [&dfs](tbb::blocked_range<size_t> r) {
                          for (size_t i = r.begin(); i < r.end(); i++) {
                                  dfs[i]->mutations.remove_invalid();
                          }
                      });
    fix_condensed_nodes(&t);
    fprintf(stderr, "%zu condensed_nodes\n",t.condensed_nodes.size());
    Mutation_Annotated_Tree::save_mutation_annotated_tree(t, output_path);
}
