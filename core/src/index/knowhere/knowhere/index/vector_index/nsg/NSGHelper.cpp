// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include <cstring>
#include <fstream>

#include "knowhere/index/vector_index/nsg/NSGHelper.h"

namespace knowhere {
namespace algo {

// TODO: impl search && insert && return insert pos. why not just find and swap?
int
InsertIntoPool(Neighbor* addr, unsigned K, Neighbor nn) {
    //>> Fix: Add assert
    for (unsigned int i = 0; i < K; ++i) {
        assert(addr[i].id != nn.id);
    }

    // find the location to insert
    int left = 0, right = K - 1;
    if (addr[left].distance > nn.distance) {
        //>> Fix: memmove overflow, dump when vector<Neighbor> deconstruct
        memmove((char*)&addr[left + 1], &addr[left], (K - 1) * sizeof(Neighbor));
        addr[left] = nn;
        return left;
    }
    if (addr[right].distance < nn.distance) {
        addr[K] = nn;
        return K;
    }
    while (left < right - 1) {
        int mid = (left + right) / 2;
        if (addr[mid].distance > nn.distance)
            right = mid;
        else
            left = mid;
    }
    // check equal ID

    while (left > 0) {
        if (addr[left].distance < nn.distance)  // pos is right
            break;
        if (addr[left].id == nn.id)
            return K + 1;
        left--;
    }
    if (addr[left].id == nn.id || addr[right].id == nn.id)
        return K + 1;

    //>> Fix: memmove overflow, dump when vector<Neighbor> deconstruct
    memmove((char*)&addr[right + 1], &addr[right], (K - 1 - right) * sizeof(Neighbor));
    addr[right] = nn;
    return right;
}

// TODO: support L2 / IP
float
calculate(const float* a, const float* b, unsigned size) {
    float result = 0;

#ifdef __GNUC__
#ifdef __AVX__

#define AVX_L2SQR(addr1, addr2, dest, tmp1, tmp2) \
    tmp1 = _mm256_loadu_ps(addr1);                \
    tmp2 = _mm256_loadu_ps(addr2);                \
    tmp1 = _mm256_sub_ps(tmp1, tmp2);             \
    tmp1 = _mm256_mul_ps(tmp1, tmp1);             \
    dest = _mm256_add_ps(dest, tmp1);

    __m256 sum;
    __m256 l0, l1;
    __m256 r0, r1;
    unsigned D = (size + 7) & ~7U;
    unsigned DR = D % 16;
    unsigned DD = D - DR;
    const float* l = a;
    const float* r = b;
    const float* e_l = l + DD;
    const float* e_r = r + DD;
    float unpack[8] __attribute__((aligned(32))) = {0, 0, 0, 0, 0, 0, 0, 0};

    sum = _mm256_loadu_ps(unpack);
    if (DR) {
        AVX_L2SQR(e_l, e_r, sum, l0, r0);
    }

    for (unsigned i = 0; i < DD; i += 16, l += 16, r += 16) {
        AVX_L2SQR(l, r, sum, l0, r0);
        AVX_L2SQR(l + 8, r + 8, sum, l1, r1);
    }
    _mm256_storeu_ps(unpack, sum);
    result = unpack[0] + unpack[1] + unpack[2] + unpack[3] + unpack[4] + unpack[5] + unpack[6] + unpack[7];

#else
#ifdef __SSE2__
#define SSE_L2SQR(addr1, addr2, dest, tmp1, tmp2) \
    tmp1 = _mm_load_ps(addr1);                    \
    tmp2 = _mm_load_ps(addr2);                    \
    tmp1 = _mm_sub_ps(tmp1, tmp2);                \
    tmp1 = _mm_mul_ps(tmp1, tmp1);                \
    dest = _mm_add_ps(dest, tmp1);

    __m128 sum;
    __m128 l0, l1, l2, l3;
    __m128 r0, r1, r2, r3;
    unsigned D = (size + 3) & ~3U;
    unsigned DR = D % 16;
    unsigned DD = D - DR;
    const float* l = a;
    const float* r = b;
    const float* e_l = l + DD;
    const float* e_r = r + DD;
    float unpack[4] __attribute__((aligned(16))) = {0, 0, 0, 0};

    sum = _mm_load_ps(unpack);
    switch (DR) {
        case 12:
            SSE_L2SQR(e_l + 8, e_r + 8, sum, l2, r2);
        case 8:
            SSE_L2SQR(e_l + 4, e_r + 4, sum, l1, r1);
        case 4:
            SSE_L2SQR(e_l, e_r, sum, l0, r0);
        default:
            break;
    }
    for (unsigned i = 0; i < DD; i += 16, l += 16, r += 16) {
        SSE_L2SQR(l, r, sum, l0, r0);
        SSE_L2SQR(l + 4, r + 4, sum, l1, r1);
        SSE_L2SQR(l + 8, r + 8, sum, l2, r2);
        SSE_L2SQR(l + 12, r + 12, sum, l3, r3);
    }
    _mm_storeu_ps(unpack, sum);
    result += unpack[0] + unpack[1] + unpack[2] + unpack[3];

// nomal distance
#else

    float diff0, diff1, diff2, diff3;
    const float* last = a + size;
    const float* unroll_group = last - 3;

    /* Process 4 items with each loop for efficiency. */
    while (a < unroll_group) {
        diff0 = a[0] - b[0];
        diff1 = a[1] - b[1];
        diff2 = a[2] - b[2];
        diff3 = a[3] - b[3];
        result += diff0 * diff0 + diff1 * diff1 + diff2 * diff2 + diff3 * diff3;
        a += 4;
        b += 4;
    }
    /* Process last 0-3 pixels.  Not needed for standard vector lengths. */
    while (a < last) {
        diff0 = *a++ - *b++;
        result += diff0 * diff0;
    }
#endif
#endif
#endif

    return result;
}

}  // namespace algo
}  // namespace knowhere