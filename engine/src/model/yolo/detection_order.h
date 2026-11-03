#pragma once
#include <algorithm>
#include <vector>

// Scores stay attached to their original box; only candidate indices are sorted.
inline void sort_detection_indices(const std::vector<float> &scores, std::vector<int> &indices) {
    std::stable_sort(indices.begin(), indices.end(), [&](int a, int b) { return scores[a] > scores[b]; });
}
