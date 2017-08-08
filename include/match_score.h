#pragma once

#include <stdint.h>
#include <vector>
#include <queue>
#include <stdlib.h>
#include <limits>

#ifdef DEBUG
#define D(x) x
#else
#define D(x)
#endif

#define TokenOffsetHeap std::priority_queue<TokenOffset, std::vector<TokenOffset>, TokenOffset>

struct TokenOffset {
    uint8_t token_id;         // token identifier
    uint16_t offset;          // token's offset in the text
    uint16_t offset_index;    // index of the offset in the vector

    bool operator() (const TokenOffset& a, const TokenOffset& b) {
        return a.offset > b.offset;
    }
};

struct MatchScore {
  uint16_t words_present;
  uint16_t distance;
  uint16_t start_offset;
  char offset_diffs[16];

  MatchScore() {

  }

  MatchScore(uint16_t words_present, uint16_t distance, uint16_t start_offset, char *offset_diffs_stacked):
          words_present(words_present), distance(distance), start_offset(start_offset) {
    memcpy(offset_diffs, offset_diffs_stacked, 16);
  }

  static void print_token_offsets(std::vector<std::vector<uint16_t>> &token_offsets) {
    for(auto offsets: token_offsets) {
      for(auto offset: offsets) {
        std::cout << offset  << ", ";
      }
      std::cout << std::endl;
    }
  }

  static inline void addTopOfHeapToWindow(TokenOffsetHeap &heap, std::queue<TokenOffset> &window,
                                           std::vector<std::vector<uint16_t>> &token_offsets, uint16_t *token_offset) {
    TokenOffset top = heap.top();
    heap.pop();
    window.push(top);
    token_offset[top.token_id] = std::min(token_offset[top.token_id], top.offset);
    top.offset_index++;

    // Must refill the heap - push the next offset of the same token
    if(top.offset_index < token_offsets[top.token_id].size()) {
        heap.push(TokenOffset{top.token_id, token_offsets[top.token_id][top.offset_index], top.offset_index});
    }
  }

  static void pack_token_offsets(const uint16_t* min_token_offset, const size_t num_tokens,
                                 const size_t start_token_index, char *offset_diffs) {
      offset_diffs[0] = (char) num_tokens;
      for(size_t i = start_token_index; i < num_tokens; i++) {
        offset_diffs[1+i] = (int8_t)(min_token_offset[i] - min_token_offset[start_token_index]);
      }
  }

  /*
  *  Given *sorted offsets* of each target token in a *single* document, generates a score that indicates:
  *  a) How many tokens are present in the document
  *  b) The proximity between the tokens in the document
  *
  *  We use a priority queue to read the offset vectors in a sorted manner, slide a window of a given size, and
  *  compute the max_match and min_displacement of target tokens across the windows.
  */
  static MatchScore match_score(uint32_t doc_id, std::vector<std::vector<uint16_t>> &token_offsets) {
    const size_t WINDOW_SIZE = 10;
    const uint16_t MAX_DISPLACEMENT = std::numeric_limits<uint16_t>::max();

    std::priority_queue<TokenOffset, std::vector<TokenOffset>, TokenOffset> heap;

    for(uint8_t token_id=0; token_id < token_offsets.size(); token_id++) {
      heap.push(TokenOffset{token_id, token_offsets[token_id].front(), 0});
    }

    // heap now contains the first occurring offset of each token in the given document

    uint16_t max_match = 0;
    uint16_t min_displacement = MAX_DISPLACEMENT;

    std::queue<TokenOffset> window;
    uint16_t token_offset[WINDOW_SIZE] = { };
    std::fill_n(token_offset, WINDOW_SIZE, MAX_DISPLACEMENT);

    // used to store token offsets of the best-matched window
    uint16_t min_token_offset[WINDOW_SIZE];
    std::fill_n(min_token_offset, WINDOW_SIZE, MAX_DISPLACEMENT);

    do {
      if(window.empty()) {
        addTopOfHeapToWindow(heap, window, token_offsets, token_offset);
      }

      D(std::cout << "Loop till window fills... doc_id: " << doc_id << std::endl;)

      // Fill the queue with tokens within a given window frame size of the start offset
      // At the same time, we also record the *last* occurrence of each token within the window
      // For e.g. if `cat` appeared at offsets 1,3 and 5, we will record `token_offset[cat] = 5`
      const uint16_t start_offset = window.front().offset;
      while(!heap.empty() && heap.top().offset < start_offset+WINDOW_SIZE) {
        addTopOfHeapToWindow(heap, window, token_offsets, token_offset);
      }

      D(std::cout << std::endl << "----" << std::endl);

      uint16_t prev_pos = MAX_DISPLACEMENT;
      uint16_t num_match = 0;
      uint16_t displacement = 0;

      for(size_t token_id=0; token_id<token_offsets.size(); token_id++) {
        // If a token appeared within the window, we would have recorded its offset
        if(token_offset[token_id] != MAX_DISPLACEMENT) {
          num_match++;
          if(prev_pos == MAX_DISPLACEMENT) { // for the first word
            prev_pos = token_offset[token_id];
            displacement = 0;
          } else {
            // Calculate the distance between the tokens within the window
            // Ideally, this should be (NUM_TOKENS - 1) when all the tokens are adjacent to each other
            D(std::cout << "prev_pos: " << prev_pos << " , curr_pos: " << token_offset[token_id] << std::endl);
            displacement += abs(token_offset[token_id]-prev_pos);
            prev_pos = token_offset[token_id];
          }
        }
      }

      D(std::cout << std::endl << "!!!displacement: " << displacement << " | num_match: " << num_match << std::endl);

      // Track the best `displacement` and `num_match` seen so far across all the windows
      // for a single token, displacement will be 0, while for 2 tokens minimum dispacement would be 1
      if(num_match > max_match || (num_match == max_match && displacement < min_displacement)) {
        min_displacement = displacement;
        // record the token positions (for highlighting)
        memcpy(min_token_offset, token_offset, token_offsets.size()*sizeof(uint16_t));
        max_match = num_match;
      }

      // As we slide the window, drop the first token of the window from the computation
      token_offset[window.front().token_id] = MAX_DISPLACEMENT;
      window.pop();
    } while(!heap.empty());

    // do run-length encoding of the min token positions/offsets
    uint16_t token_start_offset = 0;
    char offset_diffs[16];
    std::fill_n(offset_diffs, 16, 0);

    int token_index = 0;

    // identify the first token which is actually present and use that as the base for run-length encoding
    while(token_index < token_offsets.size()) {
      if(min_token_offset[token_index] != MAX_DISPLACEMENT) {
        token_start_offset = min_token_offset[token_index];
        break;
      }
      token_index++;
    }

    pack_token_offsets(min_token_offset, token_offsets.size(), token_index, offset_diffs);
    return MatchScore(max_match, min_displacement, token_start_offset, offset_diffs);
  }
};
