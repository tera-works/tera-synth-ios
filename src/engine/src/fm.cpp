#include "hsynth/dsp/fm.h"

#include <cctype>

namespace hsynth {

namespace {

// Original 4 / 6-operator algorithms (indices 0..15 are stored in presets by label).
const FmAlgorithm kClassic[16] = {
    // 4 operators
    {{0b0010, 0b0100, 0b1000, 0, 0, 0, 0, 0}, 0b0001, 3, 4},  // 4>3>2>1
    {{0b0010, 0b1100, 0, 0, 0, 0, 0, 0}, 0b0001, 3, 4},       // (4+3)>2>1
    {{0b0110, 0, 0b1000, 0, 0, 0, 0, 0}, 0b0001, 3, 4},       // 4>3>1 + 2>1
    {{0b0010, 0, 0b1000, 0, 0, 0, 0, 0}, 0b0101, 3, 4},       // 4>3 + 2>1
    {{0b1000, 0b1000, 0b1000, 0, 0, 0, 0, 0}, 0b0111, 3, 4},  // 4>(3,2,1)
    {{0, 0b0100, 0b1000, 0, 0, 0, 0, 0}, 0b0011, 3, 4},       // 4>3>2 + 1
    {{0b1000, 0, 0, 0, 0, 0, 0, 0}, 0b0111, 3, 4},            // 4>1 + 2 + 3
    {{0, 0, 0, 0, 0, 0, 0, 0}, 0b1111, 3, 4},                 // additive
    // 6 operators (DX7 numbering: op1 = carrier side, op6 = top of the stack)
    {{0b000010, 0, 0b001000, 0b010000, 0b100000, 0, 0, 0}, 0b000101, 5, 6},
    {{0b000010, 0, 0b001000, 0b010000, 0b100000, 0, 0, 0}, 0b000101, 1, 6},
    {{0b000010, 0, 0b001000, 0, 0b100000, 0, 0, 0}, 0b010101, 5, 6},
    {{0b000010, 0, 0b011000, 0, 0b100000, 0, 0, 0}, 0b000101, 5, 6},
    {{0b010110, 0, 0b001000, 0, 0b100000, 0, 0, 0}, 0b000001, 5, 6},
    {{0b000010, 0, 0b100000, 0b100000, 0b100000, 0, 0, 0}, 0b011101, 5, 6},
    {{0, 0, 0, 0, 0b100000, 0, 0, 0}, 0b011111, 5, 6},
    {{0, 0, 0, 0, 0, 0, 0, 0}, 0b111111, 5, 6},
};
const char* const kClassicNames[16] = {
    "1: 4>3>2>1", "2: (4+3)>2>1", "3: 4>3>1 + 2>1", "4: 4>3 + 2>1", "5: 4>(3,2,1)",
    "6: 4>3>2 + 1", "7: 4>1 + 2 + 3", "8: 1+2+3+4",
    "9: 6op 6>5>4>3 + 2>1", "10: 6op 6>5>4>3 + 2>1 (fb 2)", "11: 6op 6>5 + 4>3 + 2>1", "12: 6op 6>5>3, 4>3, 2>1",
    "13: 6op (2 + 4>3 + 6>5)>1", "14: 6op 6>(5,4,3) + 2>1", "15: 6op 6>5 + 1..4", "16: 6op 1+2+3+4+5+6"};

// 8-operator algorithms. Notation: chains joined by " + "; in a chain "a>b"
// means operator a modulates b (a > b numerically); "(a,b,c)" is a group
// (every member connects to every member of the neighbouring token). An
// operator that modulates nothing is a carrier. Feedback goes to the highest
// operator unless " fb N" is given.
const char* const kSpecs8[] = {
    "8>7>6>5>4>3>2>1",
    "8>7>6>5 + 4>3>2>1",
    "8>7>6>5>4>3 + 2>1",
    "8>7>6>5>4 + 3>2>1",
    "8>7>6 + 5>4>3 + 2>1",
    "8>7 + 6>5 + 4>3 + 2>1",
    "8>7>6>5>4>3>2 + 1",
    "8>7>6>5>4>3 + 2 + 1",
    "8>7>6>5 + 4>3 + 2>1",
    "8>7>6>5 + 4 + 3 + 2 + 1",
    "8>7 + 6>5 + 4>3 + 2 + 1",
    "8>7 + 6 + 5 + 4 + 3 + 2 + 1",
    "8 + 7 + 6 + 5 + 4 + 3 + 2 + 1",
    "8>7>6 + 5>4>3 + 2 + 1",
    "8>7>6>5>4 + 3 + 2 + 1",
    "8>7>6 + 5>4 + 3>2 + 1",
    "(8,7,6,5,4,3,2)>1",
    "8>7>1 + 6>5>1 + 4>3>1 + 2>1",
    "8>7>6>1 + 5>4>3>1 + 2>1",
    "(8,7)>6>5>4>3>2>1",
    "8>(7,6,5,4,3,2,1)",
    "8>7>(6,5,4,3,2,1)",
    "(8,7)>(6,5,4,3,2,1)",
    "8>(7,6,5,4) + 3>2>1",
    "8>(4,3,2,1) + 7>6>5",
    "8>7>(4,3,2,1) + 6>5",
    "8>7>6>3 + 5>4>3 + 2>1",
    "8>7>2 + 6>5>2 + 4>3>1 + 2>1",
    "8>7>6>5>1 + 4>3>2>1",
    "8>7>5 + 6>5>4>1 + 3>2>1",
    "(8,7)>6>5 + (4,3)>2>1",
    "(8,7)>6 + (5,4)>3 + 2>1",
    "8>(7,6)>5 + 4>3>2>1",
    "8>(7,6)>(5,4) + 3>2>1",
    "(8,7,6)>5>4 + (3,2)>1",
    "(8,7)>6>(5,4,3) + 2>1",
    "8>7>(6,5) + 4>3 + 2>1",
    "(8,7)>(6,5) + (4,3)>(2,1)",
    "8>(7,6) + 5>(4,3) + 2>1",
    "8>7>6>(5,4,3,2,1)",
    "(8,7,6,5)>(4,3,2,1)",
    "(8,7,6,5)>4>3>2>1",
    "8>7>6>5>(4,3,2,1)",
    "8>(7,6,5) + 4>(3,2,1)",
    "8>7>(6,5) + 4>(3,2) + 1",
    "8>7>6>5 + 8>4>3 + 2>1",
    "(8,7,6)>5 + 4>3>2>1",
    "8>4 + 7>3 + 6>2 + 5>1",
    "(8,7)>4 + (6,5)>3 + 2>1",
    "8>6>4>2 + 7>5>3>1",
    "8>6>4>2 + 7>5>3 + 1",
    "8>7>6>5 + 4>3>2>1 fb 4",
    "8>7>6>5>4>3>2>1 fb 1",
    "(8,7,6,5,4,3,2)>1 fb 1",
    "8 + 7 + 6 + 5 + 4 + 3 + 2 + 1 fb 1",
    "8>(7,6,5,4,3,2,1) fb 7",
    "8>7 + 6>5 + 4>3 + 2>1 fb 6",
    "8>7>6 + 5>4>3 + 2>1 fb 5",
    "8>7>(3,2,1) + 6>5>4",
    "8>7>6 + 8>5>4 + 3>2>1",
    "8>7>6>5 + 8>3 + 4>2>1",
    "(8,7)>6>(5,4) + (3,2)>1",
    "8>(7,6,5) + (4,3)>2>1",
    "8>6 + 7>5 + (6,5)>4>3 + 2>1",
    "8>7>6>5>4 + 8>3>2>1",
    "(8,7,6)>(5,4,3) + 2>1",
    "8>7 + 6>5 + 4>3 + (8,6,4)>2>1",
    "8>(6,4,2) + 7>(5,3,1)",
    "8>7>(5,3,1) + 6>(4,2)",
    "(8,7)>(6,5,4) + 3 + 2 + 1",
    "8>7>6>5>4>3>2 + 8>1",
    "8>7>6 + 5>4 + 3>2>1 fb 3",
};
constexpr int kNum8 = (int)(sizeof kSpecs8 / sizeof kSpecs8[0]);
static_assert(16 + kNum8 == kNumFmAlgorithms, "algorithm count");

// Parses one spec into an algorithm (asserting the "modulator above carrier" rule).
FmAlgorithm parse(const std::string& spec) {
    FmAlgorithm a{};
    int used = 0, fb = -1;
    std::string s = spec;
    if (const size_t f = s.find(" fb "); f != std::string::npos) {
        fb = std::atoi(s.c_str() + f + 4) - 1;
        s = s.substr(0, f);
    }
    // split on '+' outside parentheses
    std::vector<std::string> chains;
    {
        int depth = 0;
        std::string cur;
        for (char c : s) {
            if (c == '(') ++depth;
            if (c == ')') --depth;
            if (c == '+' && depth == 0) { chains.push_back(cur); cur.clear(); continue; }
            cur += c;
        }
        chains.push_back(cur);
    }
    uint8_t modulates = 0;  // ops that modulate something
    for (const auto& chain : chains) {
        std::vector<std::vector<int>> tokens;
        std::vector<int> cur;
        bool inGroup = false;
        for (size_t i = 0; i <= chain.size(); ++i) {
            const char c = i < chain.size() ? chain[i] : '>';
            if (std::isdigit((unsigned char)c)) {
                const int op = c - '1';
                cur.push_back(op);
                used |= 1 << op;
            } else if (c == '(') {
                inGroup = true;
            } else if (c == ')') {
                inGroup = false;
            } else if (c == '>' && !inGroup) {
                if (!cur.empty()) tokens.push_back(cur);
                cur.clear();
            }
        }
        for (size_t t = 0; t + 1 < tokens.size(); ++t)
            for (int m : tokens[t])
                for (int c : tokens[t + 1])
                    if (m > c) {
                        a.modulators[c] |= (uint8_t)(1 << m);
                        modulates |= (uint8_t)(1 << m);
                    }
    }
    int numOps = 0;
    for (int i = 0; i < P::kFmOps; ++i)
        if (used & (1 << i)) numOps = i + 1;
    a.numOps = (int8_t)numOps;
    a.carriers = (uint8_t)(used & ~modulates);
    a.feedbackOp = (int8_t)(fb >= 0 && fb < numOps ? fb : numOps - 1);
    return a;
}

struct Tables {
    std::vector<FmAlgorithm> algos;
    std::vector<std::string> names;
    Tables() {
        for (int i = 0; i < 16; ++i) {
            algos.push_back(kClassic[i]);
            names.push_back(kClassicNames[i]);
        }
        for (int i = 0; i < kNum8; ++i) {
            algos.push_back(parse(kSpecs8[i]));
            names.push_back(std::to_string(17 + i) + ": 8op " + kSpecs8[i]);
        }
    }
};

const Tables& tables() {
    static const Tables t;
    return t;
}

}  // namespace

const std::vector<std::string>& fmAlgorithmNames() { return tables().names; }

const FmAlgorithm& fmAlgorithm(int index) {
    const auto& a = tables().algos;
    if (index < 0) index = 0;
    if (index >= (int)a.size()) index = (int)a.size() - 1;
    return a[(size_t)index];
}

}  // namespace hsynth
