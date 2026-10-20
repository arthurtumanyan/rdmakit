/* zipf.c - Zipfian rank generator, see zipf.h. */
#include "zipf.h"

#include <math.h>

static double zeta(uint64_t n, double theta) {
    double sum = 0.0;
    for (uint64_t i = 1; i <= n; i++) {
        sum += 1.0 / pow((double)i, theta);
    }
    return sum;
}

int pace_zipf_init(pace_zipf *z, uint64_t n, double theta) {
    if (n == 0 || theta < 0.0 || theta >= 1.0) {
        return -1;
    }
    z->n = n;
    z->theta = theta;
    z->alpha = 1.0 / (1.0 - theta);
    z->zetan = zeta(n, theta);
    double zeta2 = zeta(n < 2 ? n : 2, theta);
    z->half_pow_theta = pow(0.5, theta);
    if (n < 2) {
        z->eta = 0.0;
    } else {
        z->eta = (1.0 - pow(2.0 / (double)n, 1.0 - theta)) / (1.0 - zeta2 / z->zetan);
    }
    return 0;
}

uint64_t pace_zipf_next(const pace_zipf *z, pace_rng *r) {
    double u = pace_rng_double(r);
    double uz = u * z->zetan;
    if (uz < 1.0) {
        return 0;
    }
    if (uz < 1.0 + z->half_pow_theta) {
        return z->n > 1 ? 1 : 0;
    }
    uint64_t rank = (uint64_t)((double)z->n * pow(z->eta * u - z->eta + 1.0, z->alpha));
    return rank < z->n ? rank : z->n - 1;
}

uint64_t pace_zipf_scramble(uint64_t rank, uint64_t n) {
    uint64_t state = rank;
    return (uint64_t)(((unsigned __int128)pace_splitmix64(&state) * n) >> 64);
}
