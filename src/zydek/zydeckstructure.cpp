#include "zydeckstructure.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <map>
#include <numeric>
#include <set>

namespace zydeck::structure {

namespace {

constexpr int kFft = 4096;   // ~0.19 s at 22 kHz: a semitone apart from 80 Hz up, for the chroma
constexpr int kHop = 2048;
constexpr int kBands = 4;    // kick and bass, low mids, mids, highs
constexpr int kTimbre = 24;
constexpr int kChroma = 12;
constexpr double kPi = 3.14159265358979323846;

using Matrix = std::vector<std::vector<double>>;
using cd = std::complex<double>;

// An in-place radix-2 FFT (n a power of 2)
void fft(std::vector<cd>& a) {
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) {
            j ^= bit;
        }
        j ^= bit;
        if (i < j) {
            std::swap(a[i], a[j]);
        }
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const cd w(std::cos(-2 * kPi / len), std::sin(-2 * kPi / len));
        for (size_t i = 0; i < n; i += len) {
            cd wk(1);
            for (size_t k = 0; k < len / 2; ++k) {
                const cd u = a[i + k];
                const cd v = a[i + k + len / 2] * wk;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                wk *= w;
            }
        }
    }
}

// The power spectrum (bins 0..n/2) of n real samples, through an FFT of n/2 complex ones
void powerSpectrum(const float* x, const std::vector<double>& window, std::vector<cd>& z, std::vector<double>& out) {
    const size_t n = window.size(), h = n / 2;
    for (size_t i = 0; i < h; ++i) {
        z[i] = cd(x[2 * i] * window[2 * i], x[2 * i + 1] * window[2 * i + 1]);
    }
    fft(z);
    for (size_t k = 0; k <= h; ++k) {
        const cd a = z[k % h], b = std::conj(z[(h - k) % h]);
        const cd even = (a + b) * 0.5, odd = (a - b) * cd(0, -0.5);
        const cd X = even + std::polar(1.0, -2 * kPi * k / n) * odd;
        out[k] = std::norm(X);
    }
}

double cosine(const std::vector<double>& a, const std::vector<double>& b, double na, double nb) {
    double dot = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        dot += a[i] * b[i];
    }
    return dot / (na * nb + 1e-9);
}

Matrix cosineSsm(const Matrix& X) {
    const size_t n = X.size();
    std::vector<double> norms(n);
    for (size_t i = 0; i < n; ++i) {
        norms[i] = std::sqrt(std::inner_product(X[i].begin(), X[i].end(), X[i].begin(), 0.0));
    }
    Matrix S(n, std::vector<double>(n));
    for (size_t i = 0; i < n; ++i) {
        for (size_t j = i; j < n; ++j) {
            S[i][j] = S[j][i] = cosine(X[i], X[j], norms[i], norms[j]);
        }
    }
    return S;
}

// Rows standardised per column (z-scores), or only centred
Matrix standardised(Matrix X, bool scale) {
    if (X.empty()) {
        return X;
    }
    const size_t d = X[0].size();
    for (size_t c = 0; c < d; ++c) {
        double mean = 0, var = 0;
        for (const auto& row : X) {
            mean += row[c];
        }
        mean /= X.size();
        for (const auto& row : X) {
            var += (row[c] - mean) * (row[c] - mean);
        }
        const double sd = std::sqrt(var / X.size()) + 1e-9;
        for (auto& row : X) {
            row[c] = scale ? (row[c] - mean) / sd : row[c] - mean;
        }
    }
    return X;
}

// Each bar beside the next (harmony moves within a phrase)
Matrix embedded(const Matrix& X) {
    Matrix out(X.size());
    for (size_t i = 0; i < X.size(); ++i) {
        out[i] = X[i];
        const auto& next = X[std::min(i + 1, X.size() - 1)];
        out[i].insert(out[i].end(), next.begin(), next.end());
    }
    return out;
}

// Foote's novelty: a Gaussian-tapered checkerboard along the diagonal (2·half bars wide), 0..1
std::vector<double> novelty(const Matrix& S, int half) {
    const int n = static_cast<int>(S.size()), w = 2 * half;
    std::vector<double> g(w);
    for (int i = 0; i < w; ++i) {
        const double x = (w == 1 ? 0 : -1.0 + 2.0 * i / (w - 1)) * 2;
        g[i] = std::exp(-0.5 * x * x);
    }
    std::vector<double> out(n + 1);
    for (int i = 0; i <= n; ++i) {
        double sum = 0;
        for (int r = 0; r < w; ++r) {
            const int sr = std::clamp(i + r - half, 0, n - 1);
            for (int c = 0; c < w; ++c) {
                const int sc = std::clamp(i + c - half, 0, n - 1);
                const double sign = (r < half) == (c < half) ? 1 : -1;
                sum += S[sr][sc] * g[r] * g[c] * sign;
            }
        }
        out[i] = std::max(0.0, sum);
    }
    const double top = *std::max_element(out.begin(), out.end());
    for (double& v : out) {
        v /= top + 1e-9;
    }
    return out;
}

// Section lengths in bars, by preference (phrases are 8 bars and their multiples), in this order
const std::vector<std::pair<int, double>> kLengths = {{4, 0.05}, {8, 0.35}, {12, 0.0}, {16, 0.35}, {24, 0.1}, {32, 0.25},
        {2, -0.25}, {6, -0.15}, {10, -0.2}, {20, -0.05}, {28, -0.1}};
constexpr double kBoundaryCost = 0.55;   // each section start costs this: fewer, stronger ones

// Section starts (bars), by dynamic programming over the novelty: 0, ..., bars
std::vector<int> segment(const std::vector<double>& nov, int bars) {
    std::vector<double> best(bars + 1, -1e9);
    std::vector<int> prev(bars + 1, 0);
    best[0] = 0;
    for (int b = 1; b <= bars; ++b) {
        for (const auto& [length, bonus] : kLengths) {
            const int a = b - length;
            if (a < 0) {
                continue;
            }
            const double gain = (b < bars ? nov[b] : 0.5) + bonus - kBoundaryCost;
            if (best[a] + gain > best[b]) {
                best[b] = best[a] + gain;
                prev[b] = a;
            }
        }
        if (b == bars) {   // the last bars needn't fit the grid
            for (int a = std::max(0, b - 40); a < b; ++a) {
                if (best[a] > best[b]) {
                    best[b] = best[a];
                    prev[b] = a;
                }
            }
        }
    }
    std::vector<int> cuts{bars};
    while (cuts.back() > 0) {
        cuts.push_back(prev[cuts.back()]);
    }
    std::reverse(cuts.begin(), cuts.end());
    return cuts;
}

double median(std::vector<double> v) {
    if (v.empty()) {
        return 0;
    }
    std::sort(v.begin(), v.end());
    const size_t m = v.size() / 2;
    return v.size() % 2 ? v[m] : (v[m - 1] + v[m]) / 2;
}

struct Bars {
    Matrix bands, timbre, chroma;   // per bar
};

struct Segmented {
    double score = -1;   // the novelty where its sections start
    std::vector<std::pair<int, int>> segs;   // bars [a, b)
    std::vector<char> letters;
    std::vector<std::string> labels;
    std::vector<double> energy;
};

Segmented segmentBars(const Bars& in) {
    Segmented out;
    const int bars = static_cast<int>(in.bands.size());
    Matrix C = in.chroma;
    for (auto& row : C) {
        const double sum = std::accumulate(row.begin(), row.end(), 0.0) + 1e-9;
        for (double& v : row) {
            v /= sum;
        }
    }
    const Matrix Sc = cosineSsm(embedded(standardised(C, false)));
    const Matrix St = cosineSsm(embedded(standardised(in.timbre, true)));
    Matrix S(bars, std::vector<double>(bars));
    for (int i = 0; i < bars; ++i) {
        for (int j = 0; j < bars; ++j) {
            S[i][j] = 0.6 * Sc[i][j] + 0.4 * St[i][j];
        }
    }
    const std::vector<double> nTone = novelty(S, 4);
    const std::vector<double> nLoud = novelty(cosineSsm(standardised(in.bands, false)), 2);
    std::vector<double> nov(bars + 1);
    for (int i = 0; i <= bars; ++i) {
        nov[i] = 0.6 * nTone[i] + 0.4 * nLoud[i];
    }
    const double top = *std::max_element(nov.begin(), nov.end()) + 1e-9;
    for (double& v : nov) {
        v /= top;
    }
    const std::vector<int> cuts = segment(nov, bars);
    for (size_t i = 0; i + 1 < cuts.size(); ++i) {
        out.segs.emplace_back(cuts[i], cuts[i + 1]);
    }
    const int n = static_cast<int>(out.segs.size());
    double score = 0;
    for (int i = 1; i < n; ++i) {
        score += nov[out.segs[i].first];
    }
    out.score = n > 1 ? score / (n - 1) : 0;

    // letters: sections clustered by how alike they are bar for bar (shifted up to 2 bars either way), merged while
    // the clusters' average likeness stays above the track's own level; A, B, C in order of first use
    const auto sim = [&](int i, int j) {
        const auto [a0, a1] = out.segs[i];
        const auto [b0, b1] = out.segs[j];
        double best = -1;
        for (int d = -2; d <= 2; ++d) {
            const int L = d >= 0 ? std::min(a1 - a0, b1 - b0 - d) : std::min(a1 - a0 + d, b1 - b0);
            const int ai = d >= 0 ? a0 : a0 - d, bi = d >= 0 ? b0 + d : b0;
            if (L < 2 || ai + L > bars || bi + L > bars) {
                continue;
            }
            double sum = 0;
            for (int k = 0; k < L; ++k) {
                sum += S[ai + k][bi + k];
            }
            best = std::max(best, sum / L);
        }
        return best;
    };
    Matrix M(n, std::vector<double>(n, 1.0));
    std::vector<double> off;
    for (int i = 0; i < n; ++i) {
        for (int j = 0; j < n; ++j) {
            if (i != j) {
                M[i][j] = sim(i, j);
                off.push_back(M[i][j]);
            }
        }
    }
    double threshold = 1.0;
    if (n > 1) {
        const double mean = std::accumulate(off.begin(), off.end(), 0.0) / off.size();
        double var = 0;
        for (double v : off) {
            var += (v - mean) * (v - mean);
        }
        threshold = median(off) + 0.75 * std::sqrt(var / off.size());
    }
    std::vector<std::vector<int>> clusters;
    for (int i = 0; i < n; ++i) {
        clusters.push_back({i});
    }
    while (clusters.size() > 1) {
        double best = -9;
        size_t bx = 0, by = 0;
        for (size_t x = 0; x < clusters.size(); ++x) {
            for (size_t y = x + 1; y < clusters.size(); ++y) {
                double sum = 0;
                for (int i : clusters[x]) {
                    for (int j : clusters[y]) {
                        sum += M[i][j];
                    }
                }
                const double v = sum / (clusters[x].size() * clusters[y].size());
                if (v > best) {
                    best = v;
                    bx = x;
                    by = y;
                }
            }
        }
        if (best < threshold) {
            break;
        }
        clusters[bx].insert(clusters[bx].end(), clusters[by].begin(), clusters[by].end());
        clusters.erase(clusters.begin() + by);
    }
    std::vector<int> of(n);
    for (const auto& c : clusters) {
        const int first = *std::min_element(c.begin(), c.end());
        for (int i : c) {
            of[i] = first;
        }
    }
    std::map<int, char> names;
    for (int i = 0; i < n; ++i) {
        if (!names.count(of[i])) {
            const int k = static_cast<int>(names.size());
            names[of[i]] = static_cast<char>(k < 26 ? 'A' + k : 'Z');
        }
        out.letters.push_back(names[of[i]]);
    }

    // labels: from the letters, the loudness and the place
    std::vector<double> low(n);
    out.energy.resize(n);
    for (int i = 0; i < n; ++i) {
        double all = 0, lo = 0;
        for (int b = out.segs[i].first; b < out.segs[i].second; ++b) {
            for (int c = 0; c < kBands; ++c) {
                all += in.bands[b][c];
            }
            lo += in.bands[b][0];
        }
        const int len = out.segs[i].second - out.segs[i].first;
        out.energy[i] = all / (len * kBands);
        low[i] = lo / len;
    }
    const double eMed = median(out.energy), lowMed = median(low);
    std::map<char, int> count;
    for (char l : out.letters) {
        ++count[l];
    }
    std::set<char> candidates;
    for (const auto& [l, c] : count) {
        if (c >= 2) {
            candidates.insert(l);
        }
    }
    if (candidates.empty()) {
        for (const auto& [l, c] : count) {
            candidates.insert(l);
        }
    }
    char chorus = 'A';
    double loudest = -1e9;
    for (char l : candidates) {   // the loudest letter that comes back
        double sum = 0;
        for (int i = 0; i < n; ++i) {
            if (out.letters[i] == l) {
                sum += out.energy[i];
            }
        }
        if (sum / count[l] > loudest) {
            loudest = sum / count[l];
            chorus = l;
        }
    }
    const auto barLoudness = [&](int b) {
        return std::accumulate(in.bands[b].begin(), in.bands[b].end(), 0.0) / kBands;
    };
    for (int i = 0; i < n; ++i) {
        const char l = out.letters[i];
        const auto [a, b] = out.segs[i];
        std::string label;
        if (i == 0 && (out.energy[i] < eMed || low[i] < lowMed - 0.3) && l != chorus) {
            label = "Intro";
        } else if (i == n - 1 && l != chorus && out.energy[i] <= eMed) {
            label = "Outro";
        } else if (l == chorus) {
            label = "Chorus";
        } else if (low[i] < lowMed - 0.8) {
            label = "Break";
        } else if (i + 1 < n && out.letters[i + 1] == chorus && barLoudness(b - 1) > barLoudness(a) + 0.1) {
            label = "Build";
        } else if (count[l] == 1 && i > 0 && i < n - 1) {
            label = "Bridge";
        } else {
            label = "Verse";
        }
        out.labels.push_back(label);
    }
    return out;
}

} // namespace

Result analyse(const std::vector<float>& mono, double rate, const std::vector<double>& beats, int gridBar,
        const std::atomic<bool>* cancel) {
    Result result;
    const auto cancelled = [cancel] { return cancel && cancel->load(); };
    if (rate <= 0 || mono.size() < static_cast<size_t>(kFft)) {
        result.error = "No audio";
        return result;
    }
    if (beats.size() < 64) {
        result.error = "Too short, or no beat grid";
        return result;
    }
    // per frame: the bands, timbre and chroma of its spectrum
    const int bins = kFft / 2 + 1;
    std::vector<int> bandOf(bins, -1), timbreOf(bins, -1), noteOf(bins, -1);
    const double edges[] = {30, 150, 600, 4000, 11000};
    for (int k = 0; k < bins; ++k) {
        const double f = k * rate / kFft;
        for (int b = 0; b < kBands; ++b) {
            if (f >= edges[b] && f < edges[b + 1]) {
                bandOf[k] = b;
            }
        }
        if (f >= 40 && f < 10000) {   // 24 bands, evenly spaced in pitch (40 Hz .. 10 kHz)
            timbreOf[k] = std::min(kTimbre - 1, static_cast<int>(std::log(f / 40) / std::log(10000.0 / 40) * kTimbre));
        }
        if (f >= 80 && f < 5000) {
            noteOf[k] = ((static_cast<int>(std::lround(12 * std::log2(f / 440.0))) % 12) + 12) % 12;
        }
    }
    std::vector<double> window(kFft);
    for (int i = 0; i < kFft; ++i) {
        window[i] = 0.5 - 0.5 * std::cos(2 * kPi * i / (kFft - 1));
    }
    const size_t frames = 1 + (mono.size() - kFft) / kHop;
    std::vector<cd> z(kFft / 2);
    std::vector<double> power(bins);
    // per beat (frames are added to the beat their middle falls in), then per bar
    const int nBeats = static_cast<int>(beats.size());
    Matrix beatBands(nBeats, std::vector<double>(kBands)), beatTimbre(nBeats, std::vector<double>(kTimbre)),
            beatChroma(nBeats, std::vector<double>(kChroma));
    std::vector<int> beatFrames(nBeats);
    int beat = 0;
    for (size_t f = 0; f < frames; ++f) {
        if ((f & 255) == 0 && cancelled()) {
            result.error = "Cancelled";
            return result;
        }
        const double t = (f * kHop + kFft / 2.0) / rate;
        while (beat + 1 < nBeats && t >= beats[beat + 1]) {
            ++beat;
        }
        if (t < beats[0] || (beat == nBeats - 1 && t >= beats[beat] + (beats[beat] - beats[beat - 1]))) {
            continue;
        }
        powerSpectrum(mono.data() + f * kHop, window, z, power);
        double band[kBands] = {}, timbre[kTimbre] = {}, chroma[kChroma] = {};
        for (int k = 0; k < bins; ++k) {
            if (bandOf[k] >= 0) {
                band[bandOf[k]] += power[k];
            }
            if (timbreOf[k] >= 0) {
                timbre[timbreOf[k]] += power[k];
            }
            if (noteOf[k] >= 0) {
                chroma[noteOf[k]] += std::sqrt(power[k]);
            }
        }
        for (int b = 0; b < kBands; ++b) {
            beatBands[beat][b] += std::log1p(1e3 * band[b]);
        }
        for (int b = 0; b < kTimbre; ++b) {
            beatTimbre[beat][b] += std::log1p(1e3 * timbre[b]);
        }
        for (int b = 0; b < kChroma; ++b) {
            beatChroma[beat][b] += chroma[b];
        }
        ++beatFrames[beat];
    }
    // each of the four beats as the first of a bar: the one whose sections change most cleanly
    const int gridFirst = ((4 - gridBar % 4) % 4 + 4) % 4;   // (the beat index the grid's bars start on)
    Segmented best;
    int bestFirst = gridFirst;
    for (int first : {gridFirst, (gridFirst + 1) % 4, (gridFirst + 2) % 4, (gridFirst + 3) % 4}) {
        if (cancelled()) {
            result.error = "Cancelled";
            return result;
        }
        Bars bars;
        for (int b0 = first; b0 + 4 <= nBeats; b0 += 4) {
            std::vector<double> bb(kBands), bt(kTimbre), bc(kChroma);
            int count = 0;
            for (int i = b0; i < b0 + 4; ++i) {
                for (int c = 0; c < kBands; ++c) {
                    bb[c] += beatBands[i][c];
                }
                for (int c = 0; c < kTimbre; ++c) {
                    bt[c] += beatTimbre[i][c];
                }
                for (int c = 0; c < kChroma; ++c) {
                    bc[c] += beatChroma[i][c];
                }
                count += beatFrames[i];
            }
            if (count > 0) {
                for (double& v : bb) {
                    v /= count;
                }
                for (double& v : bt) {
                    v /= count;
                }
                for (double& v : bc) {
                    v /= count;
                }
            }
            bars.bands.push_back(bb);
            bars.timbre.push_back(bt);
            bars.chroma.push_back(bc);
        }
        if (bars.bands.size() < 16) {
            continue;
        }
        Segmented s = segmentBars(bars);
        // (the grid's own bar start, unless another is clearly better)
        if (best.score < 0 || s.score > best.score * 1.05) {
            best = std::move(s);
            bestFirst = first;
        }
    }
    if (best.segs.empty()) {
        result.error = "Too short";
        return result;
    }
    double eLow = *std::min_element(best.energy.begin(), best.energy.end());
    double eHigh = *std::max_element(best.energy.begin(), best.energy.end());
    for (size_t i = 0; i < best.segs.size(); ++i) {
        Section s;
        s.startBeat = i == 0 ? 0 : bestFirst + 4 * best.segs[i].first;   // (a pickup before the first bar: the intro's)
        s.endBeat = i + 1 == best.segs.size() ? nBeats : bestFirst + 4 * best.segs[i].second;
        s.letter = best.letters[i];
        s.label = best.labels[i];
        s.energy = eHigh > eLow ? (best.energy[i] - eLow) / (eHigh - eLow) : 0.5;
        result.sections.push_back(s);
    }
    result.bar = (4 - bestFirst) % 4;
    result.confidence = best.score;
    result.ok = true;
    return result;
}

} // namespace zydeck::structure
