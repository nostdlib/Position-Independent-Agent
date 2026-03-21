//===-- main.cpp - Polymorphic instruction transform tool ------------------===//
//
// Generates random per-build instruction subsets and verifies that compiled
// binaries use only the allowed instruction vocabulary.  Designed for
// polymorphic shellcode: every build selects a different random N-instruction
// set, making static signature detection effectively impossible.
//
// Usage:
//   poly-transform generate --arch x86_64 --seed 0xDEAD --count 10
//   poly-transform analyze  --arch x86_64 --disasm output.disasm
//   poly-transform verify   --arch x86_64 --seed 0xDEAD --disasm output.disasm
//
// The seed is typically derived from __DATE__ via FNV-1a (same mechanism as
// the DJB2 compile-time seeding in core/algorithms/djb2.h).
//
//===----------------------------------------------------------------------===//

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

// ============================================================================
// PRNG — xorshift64 (mirrors core/math/prng.h)
// ============================================================================

class PRNG {
	uint64_t state;

public:
	explicit PRNG(uint64_t seed) : state(seed ? seed : 1) {}

	uint64_t next() {
		state ^= state << 13;
		state ^= state >> 7;
		state ^= state << 17;
		return state;
	}

	// Unbiased range [0, n)
	uint64_t range(uint64_t n) {
		if (n <= 1)
			return 0;
		return next() % n;
	}
};

// ============================================================================
// FNV-1a hash (mirrors core/algorithms/djb2.h seed generation)
// ============================================================================

static uint64_t fnv1a(const char *str) {
	uint64_t hash = 0xcbf29ce484222325ULL;
	while (*str) {
		hash ^= static_cast<uint8_t>(*str++);
		hash *= 0x100000001b3ULL;
	}
	return hash;
}

// ============================================================================
// Instruction categories
// ============================================================================

struct Category {
	const char *name;
	std::vector<std::string> instructions;
	int min_picks; // minimum selections during generation
};

struct ArchDef {
	const char *name;
	std::vector<std::string> mandatory; // always included (e.g. syscall)
	std::vector<Category> categories;
};

// clang-format off

static ArchDef defX86_64() {
	return {"x86_64", {"syscall"}, {
		{"data_move",    {"mov", "lea", "push", "pop", "movzx", "movsx", "xchg"},          2},
		{"arithmetic",   {"add", "sub", "imul", "mul", "div", "inc", "dec", "neg", "not",
		                  "adc", "sbb"},                                                    2},
		{"logic",        {"xor", "and", "or"},                                              1},
		{"shift",        {"shl", "shr", "sar", "rol", "shld", "shrd"},                      0},
		{"compare",      {"cmp", "test", "bt"},                                             1},
		{"branch",       {"jcc", "jmp"},                                                    1},
		{"control",      {"call", "ret"},                                                   1},
		{"conditional",  {"cmov", "set"},                                                   0},
		{"float",        {"fadd", "fsub", "fmul", "fdiv", "fcvt", "fcmp", "fmov", "flogic"},0},
	}};
}

static ArchDef defI386() {
	return {"i386", {"int3"}, { // int 0x80 mapped as "int" base
		{"data_move",    {"mov", "lea", "push", "pop", "movzx", "movsx", "xchg"},          2},
		{"arithmetic",   {"add", "sub", "imul", "mul", "div", "inc", "dec", "neg", "not",
		                  "adc", "sbb"},                                                    2},
		{"logic",        {"xor", "and", "or"},                                              1},
		{"shift",        {"shl", "shr", "sar", "rol", "shld", "shrd"},                      0},
		{"compare",      {"cmp", "test", "bt"},                                             1},
		{"branch",       {"jcc", "jmp"},                                                    1},
		{"control",      {"call", "ret"},                                                   1},
		{"conditional",  {"cmov", "set"},                                                   0},
		{"float",        {"fadd", "fsub", "fmul", "fdiv", "fcvt", "fcmp", "fmov", "flogic"},0},
	}};
}

static ArchDef defAArch64() {
	return {"aarch64", {"svc"}, {
		{"data_move",    {"mov", "movk", "ldr", "str", "ldp", "stp", "adr"},               2},
		{"arithmetic",   {"add", "sub", "mul", "madd", "msub", "neg", "sdiv", "udiv",
		                  "adc"},                                                           2},
		{"logic",        {"and", "orr", "eor", "bic", "mvn"},                               1},
		{"shift",        {"lsl", "lsr", "asr", "ror", "extr"},                              0},
		{"compare",      {"cmp", "cmn", "ccmp", "tst"},                                    1},
		{"branch",       {"b.cond", "b", "cbz", "cbnz", "tbz", "tbnz"},                    1},
		{"control",      {"bl", "blr", "br", "ret"},                                       1},
		{"conditional",  {"csel", "cset", "csinc", "csinv", "csneg", "cinc"},              0},
		{"float",        {"fadd", "fsub", "fmul", "fdiv", "fmadd", "fcvt", "fcmp",
		                  "fmov", "fneg"},                                                  0},
		{"extend",       {"sxtw", "sxtb", "ubfiz", "ubfx", "sbfiz", "bfi", "bfxil"},      0},
	}};
}

static ArchDef defARMv7() {
	return {"armv7", {"svc"}, {
		{"data_move",    {"mov", "movw", "ldr", "str", "ldm", "stm", "push", "pop"},       2},
		{"arithmetic",   {"add", "sub", "mul", "mla", "rsb", "adc", "sbc", "neg"},         2},
		{"logic",        {"and", "orr", "eor", "bic", "mvn"},                               1},
		{"shift",        {"lsl", "lsr", "asr", "ror"},                                      0},
		{"compare",      {"cmp", "cmn", "tst"},                                            1},
		{"branch",       {"b", "bx", "beq", "bne", "bhi", "blo", "bhs", "bls",
		                  "bge", "blt", "bgt", "ble"},                                      1},
		{"control",      {"bl", "blx"},                                                     1},
		{"conditional",  {"movne", "moveq", "movgt", "movlt"},                              0},
		{"float",        {"fadd", "fsub", "fmul", "fdiv", "fcvt", "fcmp", "fmov"},         0},
	}};
}

static ArchDef defRISCV64() {
	return {"riscv64", {"ecall"}, {
		{"data_move",    {"ld", "sd", "lw", "sw", "lb", "sb", "lh", "sh", "lbu",
		                  "lhu", "lwu", "mv", "li"},                                        2},
		{"immediate",    {"lui", "auipc"},                                                  1},
		{"arithmetic",   {"add", "addi", "sub", "mul", "div", "divu", "rem", "neg"},       2},
		{"logic",        {"xor", "xori", "and", "andi", "or", "ori", "not"},               1},
		{"shift",        {"sll", "slli", "srl", "srli", "sra", "srai"},                    0},
		{"compare",      {"slt", "slti", "sltu", "sltiu", "seqz", "snez"},                 0},
		{"branch",       {"beq", "bne", "blt", "bge", "bltu", "bgeu", "beqz", "bnez"},    1},
		{"control",      {"jal", "jalr", "j", "jr", "ret"},                                1},
		{"word",         {"addw", "addiw", "subw", "mulw", "sext.w"},                      0},
		{"float",        {"fadd.d", "fsub.d", "fmul.d", "fdiv.d", "fadd.s", "fsub.s",
		                  "fmul.s", "fdiv.s", "fcvt", "fmv", "fld", "fsd"},                0},
	}};
}

static ArchDef defRISCV32() {
	return {"riscv32", {"ecall"}, {
		{"data_move",    {"lw", "sw", "lb", "sb", "lh", "sh", "lbu", "lhu", "mv", "li"},  2},
		{"immediate",    {"lui", "auipc"},                                                  1},
		{"arithmetic",   {"add", "addi", "sub", "mul", "div", "divu", "rem", "neg"},       2},
		{"logic",        {"xor", "xori", "and", "andi", "or", "ori", "not"},               1},
		{"shift",        {"sll", "slli", "srl", "srli", "sra", "srai"},                    0},
		{"compare",      {"slt", "slti", "sltu", "sltiu", "seqz", "snez"},                 0},
		{"branch",       {"beq", "bne", "blt", "bge", "bltu", "bgeu", "beqz", "bnez"},    1},
		{"control",      {"jal", "jalr", "j", "jr", "ret"},                                1},
		{"float",        {"fadd.d", "fsub.d", "fmul.d", "fdiv.d", "fadd.s", "fsub.s",
		                  "fmul.s", "fdiv.s", "fcvt", "fmv", "fld", "fsd"},                0},
	}};
}

static ArchDef defMIPS64() {
	return {"mips64", {"syscall"}, {
		{"data_move",    {"lw", "sw", "ld", "sd", "lb", "sb", "lh", "sh", "lbu",
		                  "lhu", "lwu", "move"},                                            2},
		{"immediate",    {"lui", "li", "la"},                                               1},
		{"arithmetic",   {"add", "addi", "addu", "addiu", "sub", "subu", "mul",
		                  "mult", "div", "divu"},                                           2},
		{"logic",        {"and", "andi", "or", "ori", "xor", "xori", "nor", "not"},        1},
		{"shift",        {"sll", "sllv", "srl", "srlv", "sra", "srav"},                    0},
		{"compare",      {"slt", "slti", "sltu", "sltiu"},                                 1},
		{"branch",       {"beq", "bne", "blez", "bgtz", "bltz", "bgez"},                   1},
		{"control",      {"j", "jal", "jr", "jalr"},                                       1},
		{"float",        {"add.s", "sub.s", "mul.s", "div.s", "add.d", "sub.d",
		                  "mul.d", "div.d", "cvt", "mov.s", "mov.d"},                      0},
	}};
}

// clang-format on

static ArchDef getArchDef(const std::string &arch) {
	if (arch == "x86_64")
		return defX86_64();
	if (arch == "i386")
		return defI386();
	if (arch == "aarch64")
		return defAArch64();
	if (arch == "armv7" || arch == "armv7a")
		return defARMv7();
	if (arch == "riscv64")
		return defRISCV64();
	if (arch == "riscv32")
		return defRISCV32();
	if (arch == "mips64")
		return defMIPS64();
	fprintf(stderr, "error: unknown architecture '%s'\n", arch.c_str());
	exit(1);
}

// ============================================================================
// Instruction set generation
// ============================================================================

struct InstructionSet {
	std::string arch;
	uint64_t seed;
	int count;
	std::vector<std::string> allowed;
	std::set<std::string> allowedSet;

	void add(const std::string &insn) {
		if (allowedSet.insert(insn).second) {
			allowed.push_back(insn);
		}
	}

	bool isAllowed(const std::string &base) const {
		return allowedSet.count(base) > 0;
	}
};

static InstructionSet generate(const std::string &archName, uint64_t seed,
                               int count) {
	ArchDef def = getArchDef(archName);
	PRNG rng(seed);

	InstructionSet iset;
	iset.arch = archName;
	iset.seed = seed;
	iset.count = count;

	// 1. Add mandatory instructions
	for (auto &m : def.mandatory) {
		iset.add(m);
	}

	// 2. For each category, pick min_picks (Fisher-Yates shuffle)
	for (auto &cat : def.categories) {
		auto pool = cat.instructions;
		for (size_t i = pool.size(); i > 1; i--) {
			size_t j = rng.range(i);
			std::swap(pool[i - 1], pool[j]);
		}
		for (int i = 0; i < cat.min_picks && i < (int)pool.size(); i++) {
			iset.add(pool[i]);
		}
	}

	// 3. Distribute remaining budget randomly across all categories
	int remaining = count - (int)iset.allowed.size();
	int maxAttempts = remaining * 100; // prevent infinite loop
	while (remaining > 0 && maxAttempts-- > 0) {
		size_t catIdx = rng.range(def.categories.size());
		auto &cat = def.categories[catIdx];

		// Shuffle category pool
		auto pool = cat.instructions;
		for (size_t i = pool.size(); i > 1; i--) {
			size_t j = rng.range(i);
			std::swap(pool[i - 1], pool[j]);
		}

		// Pick first not-yet-selected instruction
		for (auto &inst : pool) {
			if (!iset.isAllowed(inst)) {
				iset.add(inst);
				remaining--;
				break;
			}
		}
	}

	return iset;
}

// ============================================================================
// Mnemonic normalization — map disassembly mnemonic to base category
// ============================================================================

static std::string getBaseX86(const std::string &m) {
	// Conditional jumps → jcc
	if (m.size() > 1 && m[0] == 'j' && m != "jmp")
		return "jcc";
	// Conditional moves → cmov
	if (m.size() > 4 && m.substr(0, 4) == "cmov")
		return "cmov";
	// Set-on-condition → set
	if (m.size() > 3 && m.substr(0, 3) == "set")
		return "set";

	// MOV family (strip size suffix)
	if (m == "movq" || m == "movl" || m == "movb" || m == "movw" ||
	    m == "movabsq")
		return "mov";

	// MOVZX family
	if (m == "movzbl" || m == "movzwl" || m == "movzbq" || m == "movzwq")
		return "movzx";
	// MOVSX family
	if (m == "movslq" || m == "movsbl" || m == "movswl" || m == "movsbq" ||
	    m == "movswq")
		return "movsx";

	// LEA
	if (m == "leaq" || m == "leal")
		return "lea";
	// PUSH/POP
	if (m == "pushq" || m == "pushl" || m == "pushw")
		return "push";
	if (m == "popq" || m == "popl" || m == "popw")
		return "pop";

	// Arithmetic — strip size suffixes
	if (m == "addq" || m == "addl" || m == "addb" || m == "addw")
		return "add";
	if (m == "subq" || m == "subl" || m == "subb" || m == "subw")
		return "sub";
	if (m == "imulq" || m == "imull")
		return "imul";
	if (m == "mulq" || m == "mull")
		return "mul";
	if (m == "divq" || m == "divl" || m == "divw")
		return "div";
	if (m == "idivq" || m == "idivl")
		return "div";
	if (m == "incq" || m == "incl" || m == "incb" || m == "incw")
		return "inc";
	if (m == "decq" || m == "decl" || m == "decb" || m == "decw")
		return "dec";
	if (m == "negq" || m == "negl" || m == "negb")
		return "neg";
	if (m == "notl" || m == "notq" || m == "notb")
		return "not";
	if (m == "adcq" || m == "adcl")
		return "adc";
	if (m == "sbbq" || m == "sbbl")
		return "sbb";

	// Logic
	if (m == "xorq" || m == "xorl" || m == "xorb" || m == "xorw")
		return "xor";
	if (m == "andq" || m == "andl" || m == "andb" || m == "andw")
		return "and";
	if (m == "orq" || m == "orl" || m == "orb" || m == "orw")
		return "or";

	// Shift/rotate
	if (m == "shlq" || m == "shll" || m == "shlb" || m == "shlw")
		return "shl";
	if (m == "shrq" || m == "shrl" || m == "shrb" || m == "shrw")
		return "shr";
	if (m == "sarq" || m == "sarl" || m == "sarb" || m == "sarw")
		return "sar";
	if (m == "rolq" || m == "roll" || m == "rolw" || m == "rolb")
		return "rol";
	if (m == "shldq" || m == "shldl")
		return "shld";
	if (m == "shrdq" || m == "shrdl")
		return "shrd";

	// Compare/test
	if (m == "cmpq" || m == "cmpl" || m == "cmpb" || m == "cmpw")
		return "cmp";
	if (m == "testq" || m == "testl" || m == "testb" || m == "testw")
		return "test";
	if (m == "btl" || m == "btq")
		return "bt";

	// Control flow
	if (m == "callq" || m == "calll")
		return "call";
	if (m == "retq" || m == "retl")
		return "ret";

	// SSE/FP data movement
	if (m == "movss" || m == "movsd" || m == "movd" || m == "movdqa" ||
	    m == "movaps" || m == "movapd" || m == "movq" /* xmm context */)
		return "fmov";

	// SSE/FP arithmetic
	if (m == "addss" || m == "addsd" || m == "addps" || m == "addpd")
		return "fadd";
	if (m == "subss" || m == "subsd" || m == "subps" || m == "subpd")
		return "fsub";
	if (m == "mulss" || m == "mulsd" || m == "mulps" || m == "mulpd")
		return "fmul";
	if (m == "divss" || m == "divsd" || m == "divps" || m == "divpd")
		return "fdiv";

	// SSE/FP compare
	if (m == "ucomisd" || m == "ucomiss" || m == "cmplesd" || m == "cmpltsd" ||
	    m == "cmpless" || m == "cmplts")
		return "fcmp";

	// SSE/FP convert
	if (m == "cvtsi2ss" || m == "cvtsi2sd" || m == "cvttss2si" ||
	    m == "cvttsd2si" || m == "cvtdq2ps" || m == "cvttps2dq")
		return "fcvt";

	// SSE/FP logic
	if (m == "andpd" || m == "andps" || m == "andnpd" || m == "andnps" ||
	    m == "orpd" || m == "orps" || m == "xorpd" || m == "xorps" ||
	    m == "pand" || m == "pandn" || m == "por" || m == "pxor")
		return "flogic";

	// Sign-extend
	if (m == "cltd" || m == "cqto" || m == "cltq" || m == "cwtl" ||
	    m == "cbtw")
		return "cltd";

	// Bit manipulation
	if (m == "bsrl" || m == "bsrq" || m == "bsfl" || m == "bsfq")
		return "bsr";
	if (m == "bswapl" || m == "bswapq")
		return "bswap";

	// Misc
	if (m == "rep")
		return "rep";
	if (m == "rdtsc")
		return "rdtsc";
	if (m == "int3")
		return "int3";
	if (m == "nop" || m == "nopl" || m == "nopw" || m == "nopq")
		return "nop";

	return m; // fallback: return as-is
}

static std::string getBaseAArch64(const std::string &m) {
	// Conditional branches: b.eq, b.ne, b.hi, etc.
	if (m.size() > 2 && m.substr(0, 2) == "b." )
		return "b.cond";
	// Loads (any variant)
	if (m.substr(0, 3) == "ldr" || m == "ldur" || m == "ldurb" ||
	    m == "ldursb" || m == "ldurh" || m == "ldursw")
		return "ldr";
	// Stores
	if (m.substr(0, 3) == "str" || m == "stur" || m == "sturb" ||
	    m == "sturh")
		return "str";
	// Float ops
	if (m == "fadd" || m == "fsub" || m == "fmul" || m == "fdiv" ||
	    m == "fmadd" || m == "fmsub" || m == "fneg" || m == "fnmul" ||
	    m == "fmov" || m == "fcmp" || m == "fcsel")
		return m; // keep as-is, they match category names
	// Convert
	if (m.substr(0, 4) == "fcvt" || m.substr(0, 4) == "scvt" ||
	    m.substr(0, 4) == "ucvt")
		return "fcvt";
	// Extensions
	if (m == "sxtw" || m == "sxtb" || m == "sxth")
		return "sxtw";
	if (m.substr(0, 4) == "ubfi" || m == "ubfx")
		return "ubfiz";
	if (m.substr(0, 4) == "sbfi")
		return "sbfiz";

	return m;
}

static std::string getBaseRISCV(const std::string &m) {
	// Compressed instructions map to their base forms
	if (m == "c.addi")
		return "addi";
	if (m == "c.li")
		return "li";
	if (m == "c.lui")
		return "lui";
	if (m == "c.slli" || m == "c.slli64")
		return "slli";
	if (m == "c.nop")
		return "nop";
	// Pseudo-instructions
	if (m == "li")
		return "li";
	if (m == "mv")
		return "mv";
	if (m == "ret")
		return "ret";
	if (m == "j")
		return "j";
	if (m == "jr")
		return "jr";
	if (m == "nop")
		return "nop";
	if (m == "not")
		return "not";
	if (m == "neg" || m == "negw")
		return "neg";
	if (m == "seqz" || m == "snez" || m == "sgtz")
		return "seqz"; // mapped to compare
	if (m == "beqz")
		return "beqz";
	if (m == "bnez")
		return "bnez";
	if (m == "blez")
		return "blez"; // mapped as branch
	if (m == "bgez")
		return "bgez";
	if (m == "bltz")
		return "bltz";
	if (m == "bgtz")
		return "bgtz";
	if (m == "zext.b")
		return "zext.b";
	if (m == "sext.w")
		return "sext.w";
	// Float — match by prefix
	if (m.size() > 4 && (m.substr(0, 4) == "fadd" || m.substr(0, 4) == "fsub" ||
	                      m.substr(0, 4) == "fmul" || m.substr(0, 4) == "fdiv")) {
		if (m.find(".d") != std::string::npos)
			return m.substr(0, 4) + ".d";
		if (m.find(".s") != std::string::npos)
			return m.substr(0, 4) + ".s";
	}
	if (m.size() > 4 && m.substr(0, 4) == "fcvt")
		return "fcvt";
	if (m.size() > 3 && m.substr(0, 3) == "fmv")
		return "fmv";
	if (m == "fld" || m == "fsd" || m == "flw" || m == "fsw")
		return m;
	// CSR instructions → misc
	if (m.substr(0, 3) == "csr")
		return "csr";

	return m;
}

static std::string getBaseMnemonic(const std::string &arch,
                                   const std::string &m) {
	if (arch == "x86_64" || arch == "i386")
		return getBaseX86(m);
	if (arch == "aarch64")
		return getBaseAArch64(m);
	if (arch == "riscv64" || arch == "riscv32")
		return getBaseRISCV(m);
	// ARM/MIPS: strip condition codes for ARMv7, return as-is for MIPS
	return m;
}

// ============================================================================
// Disassembly parsing and verification
// ============================================================================

struct AnalysisResult {
	int totalInsns = 0;
	std::map<std::string, int> rawFreq;  // raw mnemonic → count
	std::map<std::string, int> baseFreq; // base mnemonic → count
};

static AnalysisResult parseDisassembly(const std::string &arch,
                                       const std::string &filename) {
	AnalysisResult result;
	std::ifstream file(filename);
	if (!file.is_open()) {
		fprintf(stderr, "error: cannot open disassembly file '%s'\n",
		        filename.c_str());
		exit(1);
	}

	std::string line;
	while (std::getline(file, line)) {
		// Skip empty lines and section/label lines
		if (line.empty())
			continue;
		if (line[0] != ' ' && line[0] != '\t')
			continue;

		// Extract first word (mnemonic)
		size_t pos = line.find_first_not_of(" \t");
		if (pos == std::string::npos)
			continue;
		size_t end = line.find_first_of(" \t", pos);
		std::string mnemonic =
		    (end != std::string::npos) ? line.substr(pos, end - pos)
		                               : line.substr(pos);

		// Skip anything that looks like a hex address (not a mnemonic)
		if (!mnemonic.empty() && mnemonic.back() == ':')
			continue;

		result.totalInsns++;
		result.rawFreq[mnemonic]++;

		std::string base = getBaseMnemonic(arch, mnemonic);
		result.baseFreq[base]++;
	}

	return result;
}

struct VerifyResult {
	int totalInsns = 0;
	int allowedInsns = 0;
	int violationInsns = 0;
	std::map<std::string, int> violations; // base mnemonic → count
	std::map<std::string, int> used;       // base mnemonic → count (allowed)
};

static VerifyResult verify(const InstructionSet &iset,
                           const AnalysisResult &analysis) {
	VerifyResult result;
	result.totalInsns = analysis.totalInsns;

	// Mnemonics that are always allowed (nop, int3, etc.)
	std::set<std::string> alwaysAllowed = {"nop", "int3"};

	for (auto &[base, count] : analysis.baseFreq) {
		if (iset.isAllowed(base) || alwaysAllowed.count(base)) {
			result.allowedInsns += count;
			result.used[base] = count;
		} else {
			result.violationInsns += count;
			result.violations[base] = count;
		}
	}

	return result;
}

// ============================================================================
// Combinatorics — count valid instruction set combinations
// ============================================================================

static uint64_t binomial(int n, int k) {
	if (k > n || k < 0)
		return 0;
	if (k == 0 || k == n)
		return 1;
	if (k > n - k)
		k = n - k;
	uint64_t result = 1;
	for (int i = 0; i < k; i++) {
		result = result * (n - i) / (i + 1);
	}
	return result;
}

static void printCombinations(const std::string &archName) {
	ArchDef def = getArchDef(archName);

	printf("\nValid 10-instruction combinations for %s:\n\n", def.name);

	uint64_t total = 1;
	printf("  %-14s  %5s  %5s  %s\n", "Category", "Avail", "Pick",
	       "C(n,k)");
	printf("  %-14s  %5s  %5s  %s\n", "──────────────", "─────", "─────",
	       "────────────");

	for (auto &cat : def.categories) {
		if (cat.min_picks == 0)
			continue;
		int n = (int)cat.instructions.size();
		int k = cat.min_picks;
		uint64_t c = binomial(n, k);
		total *= c;
		printf("  %-14s  %5d  %5d  %llu\n", cat.name, n, k,
		       (unsigned long long)c);
	}

	printf("\n  Total valid combinations: %llu\n\n",
	       (unsigned long long)total);
}

// ============================================================================
// CLI and main
// ============================================================================

static void printUsage(const char *prog) {
	printf(
	    "Usage: %s <command> [options]\n"
	    "\n"
	    "Commands:\n"
	    "  generate    Generate a random instruction set for a build\n"
	    "  analyze     Analyze instruction usage in a disassembly\n"
	    "  verify      Verify disassembly against an instruction set\n"
	    "  combos      Print number of valid instruction set combinations\n"
	    "\n"
	    "Options:\n"
	    "  --arch <name>     Architecture (x86_64, i386, aarch64, armv7,\n"
	    "                    riscv64, riscv32, mips64)\n"
	    "  --seed <value>    Random seed (hex or decimal)\n"
	    "  --count <n>       Instructions to select (default: 10)\n"
	    "  --disasm <file>   Disassembly file (from llvm-objdump)\n"
	    "\n"
	    "Typical build integration:\n"
	    "  llvm-objdump -d --no-addresses --no-show-raw-insn output.elf "
	    "> output.disasm\n"
	    "  poly-transform generate --arch x86_64 --seed $(date +%%s)\n"
	    "  poly-transform verify   --arch x86_64 --seed $(date +%%s) --disasm "
	    "output.disasm\n",
	    prog);
}

int main(int argc, char *argv[]) {
	if (argc < 2) {
		printUsage(argv[0]);
		return 1;
	}

	const char *command = argv[1];
	std::string arch;
	std::string disasmFile;
	uint64_t seed = 0;
	int count = 10;
	bool hasSeed = false;

	for (int i = 2; i < argc; i++) {
		if (strcmp(argv[i], "--arch") == 0 && i + 1 < argc) {
			arch = argv[++i];
		} else if (strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
			seed = strtoull(argv[++i], nullptr, 0);
			hasSeed = true;
		} else if (strcmp(argv[i], "--count") == 0 && i + 1 < argc) {
			count = atoi(argv[++i]);
		} else if (strcmp(argv[i], "--disasm") == 0 && i + 1 < argc) {
			disasmFile = argv[++i];
		} else {
			fprintf(stderr, "error: unknown option '%s'\n", argv[i]);
			return 1;
		}
	}

	// ── generate ──────────────────────────────────────────────────────
	if (strcmp(command, "generate") == 0) {
		if (arch.empty()) {
			fprintf(stderr, "error: --arch required\n");
			return 1;
		}
		if (!hasSeed) {
			fprintf(stderr, "error: --seed required\n");
			return 1;
		}

		InstructionSet iset = generate(arch, seed, count);

		printf("poly-transform: generated %d-instruction set for %s "
		       "(seed=0x%llx)\n\n",
		       (int)iset.allowed.size(), arch.c_str(),
		       (unsigned long long)seed);

		printf("  Allowed instructions:\n");
		for (size_t i = 0; i < iset.allowed.size(); i++) {
			printf("    %2zu. %s\n", i + 1, iset.allowed[i].c_str());
		}
		printf("\n");

		return 0;
	}

	// ── analyze ───────────────────────────────────────────────────────
	if (strcmp(command, "analyze") == 0) {
		if (arch.empty()) {
			fprintf(stderr, "error: --arch required\n");
			return 1;
		}
		if (disasmFile.empty()) {
			fprintf(stderr, "error: --disasm required\n");
			return 1;
		}

		AnalysisResult analysis = parseDisassembly(arch, disasmFile);

		printf("poly-transform: analyzed %d instructions for %s\n\n",
		       analysis.totalInsns, arch.c_str());

		// Sort by frequency (descending)
		std::vector<std::pair<std::string, int>> sorted(
		    analysis.baseFreq.begin(), analysis.baseFreq.end());
		std::sort(sorted.begin(), sorted.end(),
		          [](auto &a, auto &b) { return a.second > b.second; });

		printf("  %-16s  %7s  %5s\n", "Base mnemonic", "Count", "%");
		printf("  %-16s  %7s  %5s\n", "────────────────", "───────",
		       "─────");
		for (auto &[base, cnt] : sorted) {
			double pct = 100.0 * cnt / analysis.totalInsns;
			printf("  %-16s  %7d  %5.1f\n", base.c_str(), cnt, pct);
		}
		printf("\n  Total unique base mnemonics: %zu\n",
		       analysis.baseFreq.size());
		printf("  Total instructions: %d\n\n", analysis.totalInsns);

		return 0;
	}

	// ── verify ────────────────────────────────────────────────────────
	if (strcmp(command, "verify") == 0) {
		if (arch.empty()) {
			fprintf(stderr, "error: --arch required\n");
			return 1;
		}
		if (!hasSeed) {
			fprintf(stderr, "error: --seed required\n");
			return 1;
		}
		if (disasmFile.empty()) {
			fprintf(stderr, "error: --disasm required\n");
			return 1;
		}

		InstructionSet iset = generate(arch, seed, count);
		AnalysisResult analysis = parseDisassembly(arch, disasmFile);
		VerifyResult vresult = verify(iset, analysis);

		printf("poly-transform: verification for %s (seed=0x%llx, count=%d)\n\n",
		       arch.c_str(), (unsigned long long)seed, count);

		printf("  Allowed set: {");
		for (size_t i = 0; i < iset.allowed.size(); i++) {
			if (i > 0)
				printf(", ");
			printf("%s", iset.allowed[i].c_str());
		}
		printf("}\n\n");

		if (vresult.violations.empty()) {
			printf("  PASS: all %d instructions use only allowed set\n\n",
			       vresult.totalInsns);
			return 0;
		}

		printf("  VIOLATIONS (%d instructions, %.1f%%):\n",
		       vresult.violationInsns,
		       100.0 * vresult.violationInsns / vresult.totalInsns);

		// Sort violations by count
		std::vector<std::pair<std::string, int>> sorted(
		    vresult.violations.begin(), vresult.violations.end());
		std::sort(sorted.begin(), sorted.end(),
		          [](auto &a, auto &b) { return a.second > b.second; });

		for (auto &[base, cnt] : sorted) {
			printf("    %-16s  %5d occurrences\n", base.c_str(), cnt);
		}

		printf("\n  Allowed instructions used (%d instructions, %.1f%%):\n",
		       vresult.allowedInsns,
		       100.0 * vresult.allowedInsns / vresult.totalInsns);

		std::vector<std::pair<std::string, int>> usedSorted(
		    vresult.used.begin(), vresult.used.end());
		std::sort(usedSorted.begin(), usedSorted.end(),
		          [](auto &a, auto &b) { return a.second > b.second; });

		for (auto &[base, cnt] : usedSorted) {
			printf("    %-16s  %5d occurrences\n", base.c_str(), cnt);
		}
		printf("\n");

		return 1; // non-zero exit = violations found
	}

	// ── combos ────────────────────────────────────────────────────────
	if (strcmp(command, "combos") == 0) {
		if (arch.empty()) {
			// Print for all architectures
			for (auto &a : {"x86_64", "i386", "aarch64", "armv7", "riscv64",
			                "riscv32", "mips64"}) {
				printCombinations(a);
			}
		} else {
			printCombinations(arch);
		}
		return 0;
	}

	fprintf(stderr, "error: unknown command '%s'\n", command);
	printUsage(argv[0]);
	return 1;
}
