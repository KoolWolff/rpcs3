#include "stdafx.h"
#include "Emu/Memory/Memory.h"
#include "Emu/System.h"
#include "Emu/IdManager.h"
#include "Emu/Cell/RawSPUThread.h"
#include "Thread.h"

#ifdef _WIN32
#include <Windows.h>
#include <Psapi.h>
#else
#ifdef __APPLE__
#define _XOPEN_SOURCE
#define __USE_GNU
#endif
#include <errno.h>
#include <signal.h>
#include <ucontext.h>
#endif

static void report_fatal_error(const std::string& msg)
{
	std::string _msg = msg + "\n"
		"HOW TO REPORT ERRORS: Check the FAQ, README, other sources.\n"
		"Please, don't send incorrect reports. Thanks for understanding.\n";

#ifdef _WIN32
	_msg += "Press (Ctrl+C) to copy this message.";
	MessageBoxA(0, _msg.c_str(), "Fatal error", MB_ICONERROR); // TODO: unicode message
#else
	std::printf("Fatal error: \n%s", _msg.c_str());
#endif
}

[[noreturn]] void catch_all_exceptions()
{
	try
	{
		throw;
	}
	catch (const std::exception& e)
	{
		report_fatal_error("Unhandled exception of type '"s + typeid(e).name() + "': "s + e.what());
	}
	catch (...)
	{
		report_fatal_error("Unhandled exception (unknown)");
	}

	std::abort();
}

enum x64_reg_t : u32
{
	X64R_RAX = 0,
	X64R_RCX,
	X64R_RDX,
	X64R_RBX,
	X64R_RSP,
	X64R_RBP,
	X64R_RSI,
	X64R_RDI,
	X64R_R8,
	X64R_R9,
	X64R_R10,
	X64R_R11,
	X64R_R12,
	X64R_R13,
	X64R_R14,
	X64R_R15,

	X64R_XMM0 = 0,
	X64R_XMM1,
	X64R_XMM2,
	X64R_XMM3,
	X64R_XMM4,
	X64R_XMM5,
	X64R_XMM6,
	X64R_XMM7,
	X64R_XMM8,
	X64R_XMM9,
	X64R_XMM10,
	X64R_XMM11,
	X64R_XMM12,
	X64R_XMM13,
	X64R_XMM14,
	X64R_XMM15,
	
	X64R_AL,
	X64R_CL,
	X64R_DL,
	X64R_BL,
	X64R_AH,
	X64R_CH,
	X64R_DH,
	X64R_BH,
	
	X64_NOT_SET,
	X64_IMM8,
	X64_IMM16,
	X64_IMM32,

	X64_BIT_O = 0x90,
	X64_BIT_NO,
	X64_BIT_C,
	X64_BIT_NC,
	X64_BIT_Z,
	X64_BIT_NZ,
	X64_BIT_BE,
	X64_BIT_NBE,
	X64_BIT_S,
	X64_BIT_NS,
	X64_BIT_P,
	X64_BIT_NP,
	X64_BIT_L,
	X64_BIT_NL,
	X64_BIT_LE,
	X64_BIT_NLE,

	X64R_ECX = X64R_CL,
};

enum x64_op_t : u32
{
	X64OP_NONE,
	X64OP_LOAD, // obtain and put the value into x64 register
	X64OP_LOAD_BE,
	X64OP_LOAD_CMP,
	X64OP_LOAD_TEST,
	X64OP_STORE, // take the value from x64 register or an immediate and use it
	X64OP_STORE_BE,
	X64OP_MOVS,
	X64OP_STOS,
	X64OP_XCHG,
	X64OP_CMPXCHG,
	X64OP_AND, // lock and [mem], ...
	X64OP_OR,  // lock or  [mem], ...
	X64OP_XOR, // lock xor [mem], ...
	X64OP_INC, // lock inc [mem]
	X64OP_DEC, // lock dec [mem]
	X64OP_ADD, // lock add [mem], ...
	X64OP_ADC, // lock adc [mem], ...
	X64OP_SUB, // lock sub [mem], ...
	X64OP_SBB, // lock sbb [mem], ...
};

void decode_x64_reg_op(const u8* code, x64_op_t& out_op, x64_reg_t& out_reg, size_t& out_size, size_t& out_length)
{
	// simple analysis of x64 code allows to reinterpret MOV or other instructions in any desired way
	out_length = 0;

	u8 rex = 0, pg2 = 0;

	bool oso = false, lock = false, repne = false, repe = false;

	enum : u8
	{
		LOCK  = 0xf0,
		REPNE = 0xf2,
		REPE  = 0xf3,
	};

	// check prefixes:
	for (;; code++, out_length++)
	{
		switch (const u8 prefix = *code)
		{
		case LOCK: // group 1
		{
			if (lock)
			{
				LOG_ERROR(MEMORY, "decode_x64_reg_op(%016llxh): LOCK prefix found twice", (size_t)code - out_length);
			}
			
			lock = true;
			continue;
		}
		case REPNE: // group 1
		{
			if (repne)
			{
				LOG_ERROR(MEMORY, "decode_x64_reg_op(%016llxh): REPNE/REPNZ prefix found twice", (size_t)code - out_length);
			}
			
			repne = true;
			continue;
		}
		case REPE: // group 1
		{
			if (repe)
			{
				LOG_ERROR(MEMORY, "decode_x64_reg_op(%016llxh): REP/REPE/REPZ prefix found twice", (size_t)code - out_length);
			}
			
			repe = true;
			continue;
		}

		case 0x2e: // group 2
		case 0x36:
		case 0x3e:
		case 0x26:
		case 0x64:
		case 0x65:
		{
			if (pg2)
			{
				LOG_ERROR(MEMORY, "decode_x64_reg_op(%016llxh): 0x%02x (group 2 prefix) found after 0x%02x", (size_t)code - out_length, prefix, pg2);
			}
			else
			{
				pg2 = prefix; // probably, segment register
			}
			continue;
		}

		case 0x66: // group 3
		{
			if (oso)
			{
				LOG_ERROR(MEMORY, "decode_x64_reg_op(%016llxh): operand-size override prefix found twice", (size_t)code - out_length);
			}
			
			oso = true;
			continue;
		}

		case 0x67: // group 4
		{
			LOG_ERROR(MEMORY, "decode_x64_reg_op(%016llxh): address-size override prefix found", (size_t)code - out_length, prefix);
			out_op = X64OP_NONE;
			out_reg = X64_NOT_SET;
			out_size = 0;
			out_length = 0;
			return;
		}

		default:
		{
			if ((prefix & 0xf0) == 0x40) // check REX prefix
			{
				if (rex)
				{
					LOG_ERROR(MEMORY, "decode_x64_reg_op(%016llxh): 0x%02x (REX prefix) found after 0x%02x", (size_t)code - out_length, prefix, rex);
				}
				else
				{
					rex = prefix;
				}
				continue;
			}
		}
		}

		break;
	}

	auto get_modRM_reg = [](const u8* code, const u8 rex) -> x64_reg_t
	{
		return (x64_reg_t)(((*code & 0x38) >> 3 | (/* check REX.R bit */ rex & 4 ? 8 : 0)) + X64R_RAX);
	};

	auto get_modRM_reg_xmm = [](const u8* code, const u8 rex) -> x64_reg_t
	{
		return (x64_reg_t)(((*code & 0x38) >> 3 | (/* check REX.R bit */ rex & 4 ? 8 : 0)) + X64R_XMM0);
	};

	auto get_modRM_reg_lh = [](const u8* code) -> x64_reg_t
	{
		return (x64_reg_t)(((*code & 0x38) >> 3) + X64R_AL);
	};

	auto get_op_size = [](const u8 rex, const bool oso) -> size_t
	{
		return rex & 8 ? 8 : (oso ? 2 : 4);
	};

	auto get_modRM_size = [](const u8* code) -> size_t
	{
		switch (*code >> 6) // check Mod
		{
		case 0: return (*code & 0x07) == 4 ? 2 : 1; // check SIB
		case 1: return (*code & 0x07) == 4 ? 3 : 2; // check SIB (disp8)
		case 2: return (*code & 0x07) == 4 ? 6 : 5; // check SIB (disp32)
		default: return 1;
		}
	};

	const u8 op1 = (out_length++, *code++), op2 = code[0], op3 = code[1];

	switch (op1)
	{
	case 0x0f:
	{
		out_length++, code++;

		switch (op2)
		{
		case 0x11:
		case 0x29:
		{
			if (!repe && !repne) // MOVUPS/MOVAPS/MOVUPD/MOVAPD xmm/m, xmm
			{
				out_op = X64OP_STORE;
				out_reg = get_modRM_reg_xmm(code, rex);
				out_size = 16;
				out_length += get_modRM_size(code);
				return;
			}
			break;
		}
		case 0x7f:
		{
			if ((repe && !oso) || (!repe && oso)) // MOVDQU/MOVDQA xmm/m, xmm
			{
				out_op = X64OP_STORE;
				out_reg = get_modRM_reg_xmm(code, rex);
				out_size = 16;
				out_length += get_modRM_size(code);
				return;
			}
			break;
		}
		case 0xb0:
		{
			if (!oso) // CMPXCHG r8/m8, r8
			{
				out_op = X64OP_CMPXCHG;
				out_reg = rex & 8 ? get_modRM_reg(code, rex) : get_modRM_reg_lh(code);
				out_size = 1;
				out_length += get_modRM_size(code);
				return;
			}
			break;
		}
		case 0xb1:
		{
			if (true) // CMPXCHG r/m, r (16, 32, 64)
			{
				out_op = X64OP_CMPXCHG;
				out_reg = get_modRM_reg(code, rex);
				out_size = get_op_size(rex, oso);
				out_length += get_modRM_size(code);
				return;
			}
			break;
		}
		case 0x90:
		case 0x91:
		case 0x92:
		case 0x93:
		case 0x94:
		case 0x95:
		case 0x96:
		case 0x97:
		case 0x98:
		case 0x9a:
		case 0x9b:
		case 0x9c:
		case 0x9d:
		case 0x9e:
		case 0x9f:
		{
			if (!lock) // SETcc
			{
				out_op = X64OP_STORE;
				out_reg = x64_reg_t(X64_BIT_O + op2 - 0x90); // 0x90 .. 0x9f
				out_size = 1;
				out_length += get_modRM_size(code);
				return;
			}
			break;
		}
		case 0x38:
		{
			out_length++, code++;

			switch (op3)
			{
			case 0xf0:
			case 0xf1:
			{
				if (!repne) // MOVBE
				{
					out_op = op3 == 0xf0 ? X64OP_LOAD_BE : X64OP_STORE_BE;
					out_reg = get_modRM_reg(code, rex);
					out_size = get_op_size(rex, oso);
					out_length += get_modRM_size(code);
					return;
				}

				break;
			}
			}

			break;
		}
		}

		break;
	}
	case 0x20:
	{
		if (!oso)
		{
			out_op = X64OP_AND;
			out_reg = rex & 8 ? get_modRM_reg(code, rex) : get_modRM_reg_lh(code);
			out_size = 1;
			out_length += get_modRM_size(code);
			return;
		}
		break;
	}
	case 0x21:
	{
		if (true)
		{
			out_op = X64OP_AND;
			out_reg = get_modRM_reg(code, rex);
			out_size = get_op_size(rex, oso);
			out_length += get_modRM_size(code);
			return;
		}
		break;
	}
	case 0x80:
	{
		switch (auto mod_code = get_modRM_reg(code, 0))
		{
		//case 0: out_op = X64OP_ADD; break; // TODO: strange info in instruction manual
		case 1: out_op = X64OP_OR; break;
		case 2: out_op = X64OP_ADC; break;
		case 3: out_op = X64OP_SBB; break;
		case 4: out_op = X64OP_AND; break;
		case 5: out_op = X64OP_SUB; break;
		case 6: out_op = X64OP_XOR; break;
		default: out_op = X64OP_LOAD_CMP; break;
		}

		out_reg = X64_IMM8;
		out_size = 1;
		out_length += get_modRM_size(code) + 1;
		return;
	}
	case 0x81:
	{
		switch (auto mod_code = get_modRM_reg(code, 0))
		{
		case 0: out_op = X64OP_ADD; break;
		case 1: out_op = X64OP_OR; break;
		case 2: out_op = X64OP_ADC; break;
		case 3: out_op = X64OP_SBB; break;
		case 4: out_op = X64OP_AND; break;
		case 5: out_op = X64OP_SUB; break;
		case 6: out_op = X64OP_XOR; break;
		default: out_op = X64OP_LOAD_CMP; break;
		}

		out_reg = oso ? X64_IMM16 : X64_IMM32;
		out_size = get_op_size(rex, oso);
		out_length += get_modRM_size(code) + (oso ? 2 : 4);
		return;
	}
	case 0x83:
	{
		switch (auto mod_code = get_modRM_reg(code, 0))
		{
		case 0: out_op = X64OP_ADD; break;
		case 1: out_op = X64OP_OR; break;
		case 2: out_op = X64OP_ADC; break;
		case 3: out_op = X64OP_SBB; break;
		case 4: out_op = X64OP_AND; break;
		case 5: out_op = X64OP_SUB; break;
		case 6: out_op = X64OP_XOR; break;
		default: out_op = X64OP_LOAD_CMP; break;
		}

		out_reg = X64_IMM8;
		out_size = get_op_size(rex, oso);
		out_length += get_modRM_size(code) + 1;
		return;
	}
	case 0x86:
	{
		if (!oso) // XCHG r8/m8, r8
		{
			out_op = X64OP_XCHG;
			out_reg = rex & 8 ? get_modRM_reg(code, rex) : get_modRM_reg_lh(code);
			out_size = 1;
			out_length += get_modRM_size(code);
			return;
		}
		break;
	}
	case 0x87:
	{
		if (true) // XCHG r/m, r (16, 32, 64)
		{
			out_op = X64OP_XCHG;
			out_reg = get_modRM_reg(code, rex);
			out_size = get_op_size(rex, oso);
			out_length += get_modRM_size(code);
			return;
		}
		break;
	}
	case 0x88:
	{
		if (!lock && !oso) // MOV r8/m8, r8
		{
			out_op = X64OP_STORE;
			out_reg = rex & 8 ? get_modRM_reg(code, rex) : get_modRM_reg_lh(code);
			out_size = 1;
			out_length += get_modRM_size(code);
			return;
		}
		break;
	}
	case 0x89:
	{
		if (!lock) // MOV r/m, r (16, 32, 64)
		{
			out_op = X64OP_STORE;
			out_reg = get_modRM_reg(code, rex);
			out_size = get_op_size(rex, oso);
			out_length += get_modRM_size(code);
			return;
		}
		break;
	}
	case 0x8a:
	{
		if (!lock && !oso) // MOV r8, r8/m8
		{
			out_op = X64OP_LOAD;
			out_reg = rex & 8 ? get_modRM_reg(code, rex) : get_modRM_reg_lh(code);
			out_size = 1;
			out_length += get_modRM_size(code);
			return;
		}
		break;
	}
	case 0x8b:
	{
		if (!lock) // MOV r, r/m (16, 32, 64)
		{
			out_op = X64OP_LOAD;
			out_reg = get_modRM_reg(code, rex);
			out_size = get_op_size(rex, oso);
			out_length += get_modRM_size(code);
			return;
		}
		break;
	}
	case 0xa4:
	{
		if (!oso && !lock && !repe && !rex) // MOVS
		{
			out_op = X64OP_MOVS;
			out_reg = X64_NOT_SET;
			out_size = 1;
			return;
		}
		if (!oso && !lock && repe) // REP MOVS
		{
			out_op = X64OP_MOVS;
			out_reg = rex & 8 ? X64R_RCX : X64R_ECX;
			out_size = 1;
			return;
		}
		break;
	}
	case 0xaa:
	{
		if (!oso && !lock && !repe && !rex) // STOS
		{
			out_op = X64OP_STOS;
			out_reg = X64_NOT_SET;
			out_size = 1;
			return;
		}
		if (!oso && !lock && repe) // REP STOS
		{
			out_op = X64OP_STOS;
			out_reg = rex & 8 ? X64R_RCX : X64R_ECX;
			out_size = 1;
			return;
		}
		break;
	}
	case 0xc4: // 3-byte VEX prefix
	case 0xc5: // 2-byte VEX prefix
	{
		// Last prefix byte: op2 or op3
		const u8 opx = op1 == 0xc5 ? op2 : op3;

		// Implied prefixes
		rex |= op2 & 0x80 ? 0 : 0x4; // REX.R
		rex |= op1 == 0xc4 && op3 & 0x80 ? 0x8 : 0; // REX.W ???
		oso = (opx & 0x3) == 0x1;
		repe = (opx & 0x3) == 0x2;
		repne = (opx & 0x3) == 0x3;

		const u8 vopm = op1 == 0xc5 ? 1 : op2 & 0x1f;
		const u8 vop1 = op1 == 0xc5 ? op3 : code[2];
		const u8 vlen = (opx & 0x4) ? 32 : 16;
		const u8 vreg = (~opx >> 3) & 0xf;
		out_length += op1 == 0xc5 ? 2 : 3;
		code += op1 == 0xc5 ? 2 : 3;

		if (vopm == 0x1) switch (vop1) // Implied leading byte 0x0F
		{
		case 0x11:
		case 0x29:
		{
			if (!repe && !repne) // VMOVAPS/VMOVAPD/VMOVUPS/VMOVUPD mem,reg
			{
				out_op = X64OP_STORE;
				out_reg = get_modRM_reg_xmm(code, rex);
				out_size = vlen;
				out_length += get_modRM_size(code);
				return;
			}
			break;
		}
		case 0x7f:
		{
			if (repe || oso) // VMOVDQU/VMOVDQA mem,reg
			{
				out_op = X64OP_STORE;
				out_reg = get_modRM_reg_xmm(code, rex);
				out_size = vlen;
				out_length += get_modRM_size(code);
				return;
			}
			break;
		}
		}

		break;
	}
	case 0xc6:
	{
		if (!lock && !oso && get_modRM_reg(code, 0) == 0) // MOV r8/m8, imm8
		{
			out_op = X64OP_STORE;
			out_reg = X64_IMM8;
			out_size = 1;
			out_length += get_modRM_size(code) + 1;
			return;
		}
		break;
	}
	case 0xc7:
	{
		if (!lock && get_modRM_reg(code, 0) == 0) // MOV r/m, imm16/imm32 (16, 32, 64)
		{
			out_op = X64OP_STORE;
			out_reg = oso ? X64_IMM16 : X64_IMM32;
			out_size = get_op_size(rex, oso);
			out_length += get_modRM_size(code) + (oso ? 2 : 4);
			return;
		}
		break;
	}
	case 0xf6:
	{
		switch (auto mod_code = get_modRM_reg(code, 0))
		{
		case 0: out_op = X64OP_LOAD_TEST; break;
		default: out_op = X64OP_NONE; break; // TODO...
		}

		out_reg = X64_IMM8;
		out_size = 1;
		out_length += get_modRM_size(code) + 1;
		return;
	}
	case 0xf7:
	{
		switch (auto mod_code = get_modRM_reg(code, 0))
		{
		case 0: out_op = X64OP_LOAD_TEST; break;
		default: out_op = X64OP_NONE; break; // TODO...
		}

		out_reg = oso ? X64_IMM16 : X64_IMM32;
		out_size = get_op_size(rex, oso);
		out_length += get_modRM_size(code) + (oso ? 2 : 4);
		return;
	}
	}

	out_op = X64OP_NONE;
	out_reg = X64_NOT_SET;
	out_size = 0;
	out_length = 0;
}

#ifdef _WIN32

typedef CONTEXT x64_context;

#define X64REG(context, reg) (&(&(context)->Rax)[reg])
#define XMMREG(context, reg) (reinterpret_cast<v128*>(&(&(context)->Xmm0)[reg]))
#define EFLAGS(context) ((context)->EFlags)

#define ARG1(context) RCX(context)
#define ARG2(context) RDX(context)

#else

typedef ucontext_t x64_context;

#ifdef __APPLE__

#define X64REG(context, reg) (darwin_x64reg(context, reg))
#define XMMREG(context, reg) (reinterpret_cast<v128*>(&(context)->uc_mcontext->__fs.__fpu_xmm0.__xmm_reg[reg]))
#define EFLAGS(context) ((context)->uc_mcontext->__ss.__rflags)

uint64_t* darwin_x64reg(x64_context *context, int reg)
{
	auto *state = &context->uc_mcontext->__ss;
	switch(reg)
	{
	case 0: return &state->__rax;
	case 1: return &state->__rcx;
	case 2: return &state->__rdx;
	case 3: return &state->__rbx;
	case 4: return &state->__rsp;
	case 5: return &state->__rbp;
	case 6: return &state->__rsi;
	case 7: return &state->__rdi;
	case 8: return &state->__r8;
	case 9: return &state->__r9;
	case 10: return &state->__r10;
	case 11: return &state->__r11;
	case 12: return &state->__r12;
	case 13: return &state->__r13;
	case 14: return &state->__r14;
	case 15: return &state->__r15;
	case 16: return &state->__rip;
	default:
		LOG_ERROR(GENERAL, "Invalid register index: %d", reg);
		return nullptr;
	}
}

#else

static const decltype(REG_RAX) reg_table[] =
{
	REG_RAX, REG_RCX, REG_RDX, REG_RBX, REG_RSP, REG_RBP, REG_RSI, REG_RDI,
	REG_R8, REG_R9, REG_R10, REG_R11, REG_R12, REG_R13, REG_R14, REG_R15, REG_RIP
};

#define X64REG(context, reg) (&(context)->uc_mcontext.gregs[reg_table[reg]])
#define XMMREG(context, reg) (reinterpret_cast<v128*>(&(context)->uc_mcontext.fpregs->_xmm[reg]))
#define EFLAGS(context) ((context)->uc_mcontext.gregs[REG_EFL])

#endif // __APPLE__

#define ARG1(context) RDI(context)
#define ARG2(context) RSI(context)

#endif

#define RAX(c) (*X64REG((c), 0))
#define RCX(c) (*X64REG((c), 1))
#define RDX(c) (*X64REG((c), 2))
#define RSP(c) (*X64REG((c), 4))
#define RSI(c) (*X64REG((c), 6))
#define RDI(c) (*X64REG((c), 7))
#define RIP(c) (*X64REG((c), 16))

bool get_x64_reg_value(x64_context* context, x64_reg_t reg, size_t d_size, size_t i_size, u64& out_value)
{
	// get x64 reg value (for store operations)
	if (reg - X64R_RAX < 16)
	{
		// load the value from x64 register
		const u64 reg_value = *X64REG(context, reg - X64R_RAX);

		switch (d_size)
		{
		case 1: out_value = (u8)reg_value; return true;
		case 2: out_value = (u16)reg_value; return true;
		case 4: out_value = (u32)reg_value; return true;
		case 8: out_value = reg_value; return true;
		}
	}
	else if (reg - X64R_AL < 4 && d_size == 1)
	{
		out_value = (u8)(*X64REG(context, reg - X64R_AL));
		return true;
	}
	else if (reg - X64R_AH < 4 && d_size == 1)
	{
		out_value = (u8)(*X64REG(context, reg - X64R_AH) >> 8);
		return true;
	}
	else if (reg == X64_IMM8)
	{
		// load the immediate value (assuming it's at the end of the instruction)
		const s8 imm_value = *(s8*)(RIP(context) + i_size - 1);

		switch (d_size)
		{
		case 1: out_value = (u8)imm_value; return true;
		case 2: out_value = (u16)imm_value; return true; // sign-extended
		case 4: out_value = (u32)imm_value; return true; // sign-extended
		case 8: out_value = (u64)imm_value; return true; // sign-extended
		}
	}
	else if (reg == X64_IMM16)
	{
		const s16 imm_value = *(s16*)(RIP(context) + i_size - 2);

		switch (d_size)
		{
		case 2: out_value = (u16)imm_value; return true;
		}
	}
	else if (reg == X64_IMM32)
	{
		const s32 imm_value = *(s32*)(RIP(context) + i_size - 4);
		
		switch (d_size)
		{
		case 4: out_value = (u32)imm_value; return true;
		case 8: out_value = (u64)imm_value; return true; // sign-extended
		}
	}
	else if (reg == X64R_ECX)
	{
		out_value = (u32)RCX(context);
		return true;
	}
	else if (reg >= X64_BIT_O && reg <= X64_BIT_NLE)
	{
		const u32 _cf = EFLAGS(context) & 0x1;
		const u32 _zf = EFLAGS(context) & 0x40;
		const u32 _sf = EFLAGS(context) & 0x80;
		const u32 _of = EFLAGS(context) & 0x800;
		const u32 _pf = EFLAGS(context) & 0x4;
		const u32 _l = (_sf << 4) ^ _of; // SF != OF

		switch (reg & ~1)
		{
		case X64_BIT_O: out_value = !!_of ^ (reg & 1); break;
		case X64_BIT_C: out_value = !!_cf ^ (reg & 1); break;
		case X64_BIT_Z: out_value = !!_zf ^ (reg & 1); break;
		case X64_BIT_BE: out_value = !!(_cf | _zf) ^ (reg & 1); break;
		case X64_BIT_S: out_value = !!_sf ^ (reg & 1); break;
		case X64_BIT_P: out_value = !!_pf ^ (reg & 1); break;
		case X64_BIT_L: out_value = !!_l ^ (reg & 1); break;
		case X64_BIT_LE: out_value = !!(_l | _zf) ^ (reg & 1); break;
		}

		return true;
	}

	LOG_ERROR(MEMORY, "get_x64_reg_value(): invalid arguments (reg=%d, d_size=%lld, i_size=%lld)", (u32)reg, d_size, i_size);
	return false;
}

bool put_x64_reg_value(x64_context* context, x64_reg_t reg, size_t d_size, u64 value)
{
	// save x64 reg value (for load operations)
	if (reg - X64R_RAX < 16)
	{
		// save the value into x64 register
		switch (d_size)
		{
		case 1: *X64REG(context, reg - X64R_RAX) = value & 0xff | *X64REG(context, reg - X64R_RAX) & 0xffffff00; return true;
		case 2: *X64REG(context, reg - X64R_RAX) = value & 0xffff | *X64REG(context, reg - X64R_RAX) & 0xffff0000; return true;
		case 4: *X64REG(context, reg - X64R_RAX) = value & 0xffffffff; return true;
		case 8: *X64REG(context, reg - X64R_RAX) = value; return true;
		}
	}

	LOG_ERROR(MEMORY, "put_x64_reg_value(): invalid destination (reg=%d, d_size=%lld, value=0x%llx)", (u32)reg, d_size, value);
	return false;
}

bool set_x64_cmp_flags(x64_context* context, size_t d_size, u64 x, u64 y, bool carry = true)
{
	switch (d_size)
	{
	case 1: break;
	case 2: break;
	case 4: break;
	case 8: break;
	default: LOG_ERROR(MEMORY, "set_x64_cmp_flags(): invalid d_size (%lld)", d_size); return false;
	}

	const u64 sign = 1ull << (d_size * 8 - 1); // sign mask
	const u64 diff = x - y;
	const u64 summ = x + y;

	if (carry && ((x & y) | ((x ^ y) & ~summ)) & sign)
	{
		EFLAGS(context) |= 0x1; // set CF
	}
	else if (carry)
	{
		EFLAGS(context) &= ~0x1; // clear CF
	}

	if (x == y)
	{
		EFLAGS(context) |= 0x40; // set ZF
	}
	else
	{
		EFLAGS(context) &= ~0x40; // clear ZF
	}

	if (diff & sign)
	{
		EFLAGS(context) |= 0x80; // set SF
	}
	else
	{
		EFLAGS(context) &= ~0x80; // clear SF
	}

	if ((x ^ summ) & (y ^ summ) & sign)
	{
		EFLAGS(context) |= 0x800; // set OF
	}
	else
	{
		EFLAGS(context) &= ~0x800; // clear OF
	}

	const u8 p1 = (u8)diff ^ ((u8)diff >> 4);
	const u8 p2 = p1 ^ (p1 >> 2);
	const u8 p3 = p2 ^ (p2 >> 1);

	if ((p3 & 1) == 0)
	{
		EFLAGS(context) |= 0x4; // set PF
	}
	else
	{
		EFLAGS(context) &= ~0x4; // clear PF
	}

	if (((x & y) | ((x ^ y) & ~summ)) & 0x8)
	{
		EFLAGS(context) |= 0x10; // set AF
	}
	else
	{
		EFLAGS(context) &= ~0x10; // clear AF
	}

	return true;
}

size_t get_x64_access_size(x64_context* context, x64_op_t op, x64_reg_t reg, size_t d_size, size_t i_size)
{
	if (op == X64OP_MOVS || op == X64OP_STOS)
	{
		if (EFLAGS(context) & 0x400 /* direction flag */)
		{
			// skip reservation bound check (TODO)
			return 0;
		}

		if (reg != X64_NOT_SET) // get "full" access size from RCX register
		{
			u64 counter;
			if (!get_x64_reg_value(context, reg, 8, i_size, counter))
			{
				return -1;
			}

			return d_size * counter;
		}
	}

	if (op == X64OP_CMPXCHG)
	{
		// Detect whether the instruction can't actually modify memory to avoid breaking reservation
		u64 cmp, exch;
		if (!get_x64_reg_value(context, reg, d_size, i_size, cmp) || !get_x64_reg_value(context, X64R_RAX, d_size, i_size, exch))
		{
			return -1;
		}

		if (cmp == exch)
		{
			// skip reservation bound check
			return 0;
		}
	}

	return d_size;
}

namespace rsx
{
	extern std::function<bool(u32 addr, bool is_writing)> g_access_violation_handler;
}

bool handle_access_violation(u32 addr, bool is_writing, x64_context* context)
{
	if (rsx::g_access_violation_handler && rsx::g_access_violation_handler(addr, is_writing))
	{
		return true;
	}

	auto code = (const u8*)RIP(context);

	x64_op_t op;
	x64_reg_t reg;
	size_t d_size;
	size_t i_size;

	// decode single x64 instruction that causes memory access
	decode_x64_reg_op(code, op, reg, d_size, i_size);

	auto report_opcode = [=]()
	{
		if (op == X64OP_NONE)
		{
			LOG_ERROR(MEMORY, "decode_x64_reg_op(%ph): unsupported opcode: %s", code, *(be_t<v128, 1>*)(code));
		}
	};

	if ((d_size | d_size + addr) >= 0x100000000ull)
	{
		LOG_ERROR(MEMORY, "Invalid d_size (0x%llx)", d_size);
		report_opcode();
		return false;
	}

	// get length of data being accessed
	size_t a_size = get_x64_access_size(context, op, reg, d_size, i_size);

	if ((a_size | a_size + addr) >= 0x100000000ull)
	{
		LOG_ERROR(MEMORY, "Invalid a_size (0x%llx)", a_size);
		report_opcode();
		return false;
	}

	// check if address is RawSPU MMIO register
	if (addr - RAW_SPU_BASE_ADDR < (6 * RAW_SPU_OFFSET) && (addr % RAW_SPU_OFFSET) >= RAW_SPU_PROB_OFFSET)
	{
		auto thread = idm::get<RawSPUThread>((addr - RAW_SPU_BASE_ADDR) / RAW_SPU_OFFSET);

		if (!thread)
		{
			return false;
		}

		if (a_size != 4 || !d_size || !i_size)
		{
			LOG_ERROR(MEMORY, "Invalid or unsupported instruction (op=%d, reg=%d, d_size=%lld, a_size=0x%llx, i_size=%lld)", (u32)op, (u32)reg, d_size, a_size, i_size);
			report_opcode();
			return false;
		}

		switch (op)
		{
		case X64OP_LOAD:
		case X64OP_LOAD_BE:
		case X64OP_LOAD_CMP:
		case X64OP_LOAD_TEST:
		{
			u32 value;
			if (is_writing || !thread->read_reg(addr, value))
			{
				return false;
			}

			if (op != X64OP_LOAD_BE)
			{
				value = se_storage<u32>::swap(value);
			}

			if (op == X64OP_LOAD_CMP)
			{
				u64 rvalue;
				if (!get_x64_reg_value(context, reg, d_size, i_size, rvalue) || !set_x64_cmp_flags(context, d_size, value, rvalue))
				{
					return false;
				}

				break;
			}

			if (op == X64OP_LOAD_TEST)
			{
				u64 rvalue;
				if (!get_x64_reg_value(context, reg, d_size, i_size, rvalue) || !set_x64_cmp_flags(context, d_size, value & rvalue, 0))
				{
					return false;
				}

				break;
			}

			if (!put_x64_reg_value(context, reg, d_size, value))
			{
				return false;
			}

			break;
		}
		case X64OP_STORE:
		case X64OP_STORE_BE:
		{
			u64 reg_value;
			if (!is_writing || !get_x64_reg_value(context, reg, d_size, i_size, reg_value))
			{
				return false;
			}

			if (!thread->write_reg(addr, op == X64OP_STORE ? se_storage<u32>::swap((u32)reg_value) : (u32)reg_value))
			{
				return false;
			}

			break;
		}
		case X64OP_MOVS: // possibly, TODO
		case X64OP_STOS:
		default:
		{
			LOG_ERROR(MEMORY, "Invalid or unsupported operation (op=%d, reg=%d, d_size=%lld, i_size=%lld)", (u32)op, (u32)reg, d_size, i_size);
			report_opcode();
			return false;
		}
		}

		// skip processed instruction
		RIP(context) += i_size;
		return true;
	}

	// check if fault is caused by the reservation
	return vm::reservation_query(addr, (u32)a_size, is_writing, [&]() -> bool
	{
		// write memory using "privileged" access to avoid breaking reservation
		if (!d_size || !i_size)
		{
			LOG_ERROR(MEMORY, "Invalid or unsupported instruction (op=%d, reg=%d, d_size=%lld, a_size=0x%llx, i_size=%lld)", (u32)op, (u32)reg, d_size, a_size, i_size);
			report_opcode();
			return false;
		}

		switch (op)
		{
		case X64OP_STORE:
		case X64OP_STORE_BE:
		{
			if (d_size == 16 && op == X64OP_STORE)
			{
				if (reg - X64R_XMM0 >= 16)
				{
					LOG_ERROR(MEMORY, "X64OP_STORE: d_size=16, reg=%d", (u32)reg);
					return false;
				}

				std::memcpy(vm::base_priv(addr), XMMREG(context, reg - X64R_XMM0), 16);
				break;
			}

			u64 reg_value;
			if (!get_x64_reg_value(context, reg, d_size, i_size, reg_value))
			{
				return false;
			}

			if (op == X64OP_STORE_BE && d_size == 2)
			{
				reg_value = se_storage<u16>::swap((u16)reg_value);
			}
			else if (op == X64OP_STORE_BE && d_size == 4)
			{
				reg_value = se_storage<u32>::swap((u32)reg_value);
			}
			else if (op == X64OP_STORE_BE && d_size == 8)
			{
				reg_value = se_storage<u64>::swap(reg_value);
			}
			else if (op == X64OP_STORE_BE)
			{
				return false;	
			}

			if (d_size == 1)
			{
				*(volatile u8*)vm::base_priv(addr) = (u8)reg_value;
			}
			else if (d_size == 2 && addr % 2 == 0)
			{
				*(volatile u16*)vm::base_priv(addr) = (u16)reg_value; 
			}
			else if (d_size == 4 && addr % 4 == 0)
			{
				*(volatile u32*)vm::base_priv(addr) = (u32)reg_value;
			}
			else if (d_size == 8 && addr % 8 == 0)
			{
				*(volatile u64*)vm::base_priv(addr) = (u64)reg_value;
			}
			else
			{
				std::memcpy(vm::base_priv(addr), &reg_value, d_size);
			}
			
			break;
		}
		case X64OP_MOVS:
		{
			if (d_size > 8)
			{
				LOG_ERROR(MEMORY, "X64OP_MOVS: d_size=%lld", d_size);
				return false;
			}

			if (vm::base(addr) != (void*)RDI(context))
			{
				LOG_ERROR(MEMORY, "X64OP_MOVS: rdi=0x%llx, rsi=0x%llx, addr=0x%x", (u64)RDI(context), (u64)RSI(context), addr);
				return false;
			}

			u32 a_addr = addr;

			while (a_addr >> 12 == addr >> 12)
			{
				u64 value;

				// copy data
				std::memcpy(&value, (void*)RSI(context), d_size);
				std::memcpy(vm::base_priv(a_addr), &value, d_size);

				// shift pointers
				if (EFLAGS(context) & 0x400 /* direction flag */)
				{
					LOG_ERROR(MEMORY, "X64OP_MOVS TODO: reversed direction");
					return false;
					//RSI(context) -= d_size;
					//RDI(context) -= d_size;
					//a_addr -= (u32)d_size;
				}
				else
				{
					RSI(context) += d_size;
					RDI(context) += d_size;
					a_addr += (u32)d_size;
				}

				// decrement counter
				if (reg == X64_NOT_SET || !--RCX(context))
				{
					break;
				}
			}

			if (reg == X64_NOT_SET || !RCX(context))
			{
				break;
			}

			// don't skip partially processed instruction
			return true;
		}
		case X64OP_STOS:
		{
			if (d_size > 8)
			{
				LOG_ERROR(MEMORY, "X64OP_STOS: d_size=%lld", d_size);
				return false;
			}

			if (vm::base(addr) != (void*)RDI(context))
			{
				LOG_ERROR(MEMORY, "X64OP_STOS: rdi=0x%llx, addr=0x%x", (u64)RDI(context), addr);
				return false;
			}

			u64 value;
			if (!get_x64_reg_value(context, X64R_RAX, d_size, i_size, value))
			{
				return false;
			}

			u32 a_addr = addr;

			while (a_addr >> 12 == addr >> 12)
			{
				// fill data with value
				std::memcpy(vm::base_priv(a_addr), &value, d_size);

				// shift pointers
				if (EFLAGS(context) & 0x400 /* direction flag */)
				{
					LOG_ERROR(MEMORY, "X64OP_STOS TODO: reversed direction");
					return false;
					//RDI(context) -= d_size;
					//a_addr -= (u32)d_size;
				}
				else
				{
					RDI(context) += d_size;
					a_addr += (u32)d_size;
				}

				// decrement counter
				if (reg == X64_NOT_SET || !--RCX(context))
				{
					break;
				}
			}

			if (reg == X64_NOT_SET || !RCX(context))
			{
				break;
			}

			// don't skip partially processed instruction
			return true;
		}
		case X64OP_XCHG:
		{
			u64 reg_value;
			if (!get_x64_reg_value(context, reg, d_size, i_size, reg_value))
			{
				return false;
			}

			switch (d_size)
			{
			case 1: reg_value = ((atomic_t<u8>*)vm::base_priv(addr))->exchange((u8)reg_value); break;
			case 2: reg_value = ((atomic_t<u16>*)vm::base_priv(addr))->exchange((u16)reg_value); break;
			case 4: reg_value = ((atomic_t<u32>*)vm::base_priv(addr))->exchange((u32)reg_value); break;
			case 8: reg_value = ((atomic_t<u64>*)vm::base_priv(addr))->exchange((u64)reg_value); break;
			default: return false;
			}

			if (!put_x64_reg_value(context, reg, d_size, reg_value))
			{
				return false;
			}
			break;
		}
		case X64OP_CMPXCHG:
		{
			u64 reg_value, old_value, cmp_value;
			if (!get_x64_reg_value(context, reg, d_size, i_size, reg_value) || !get_x64_reg_value(context, X64R_RAX, d_size, i_size, cmp_value))
			{
				return false;
			}

			switch (d_size)
			{
			case 1: old_value = ((atomic_t<u8>*)vm::base_priv(addr))->compare_and_swap((u8)cmp_value, (u8)reg_value); break;
			case 2: old_value = ((atomic_t<u16>*)vm::base_priv(addr))->compare_and_swap((u16)cmp_value, (u16)reg_value); break;
			case 4: old_value = ((atomic_t<u32>*)vm::base_priv(addr))->compare_and_swap((u32)cmp_value, (u32)reg_value); break;
			case 8: old_value = ((atomic_t<u64>*)vm::base_priv(addr))->compare_and_swap((u64)cmp_value, (u64)reg_value); break;
			default: return false;
			}

			if (!put_x64_reg_value(context, X64R_RAX, d_size, old_value) || !set_x64_cmp_flags(context, d_size, cmp_value, old_value))
			{
				return false;
			}
			break;
		}
		case X64OP_AND:
		{
			u64 value;
			if (!get_x64_reg_value(context, reg, d_size, i_size, value))
			{
				return false;
			}

			switch (d_size)
			{
			case 1: value = *(atomic_t<u8>*)vm::base_priv(addr) &= (u8)value; break;
			case 2: value = *(atomic_t<u16>*)vm::base_priv(addr) &= (u16)value; break;
			case 4: value = *(atomic_t<u32>*)vm::base_priv(addr) &= (u32)value; break;
			case 8: value = *(atomic_t<u64>*)vm::base_priv(addr) &= (u64)value; break;
			default: return false;
			}

			if (!set_x64_cmp_flags(context, d_size, value, 0))
			{
				return false;
			}
			break;
		}
		case X64OP_OR:
		{
			u64 value;
			if (!get_x64_reg_value(context, reg, d_size, i_size, value))
			{
				return false;
			}

			switch (d_size)
			{
			case 1: value = *(atomic_t<u8>*)vm::base_priv(addr) |= (u8)value; break;
			case 2: value = *(atomic_t<u16>*)vm::base_priv(addr) |= (u16)value; break;
			case 4: value = *(atomic_t<u32>*)vm::base_priv(addr) |= (u32)value; break;
			case 8: value = *(atomic_t<u64>*)vm::base_priv(addr) |= (u64)value; break;
			default: return false;
			}

			if (!set_x64_cmp_flags(context, d_size, value, 0))
			{
				return false;
			}
			break;
		}
		case X64OP_XOR:
		{
			u64 value;
			if (!get_x64_reg_value(context, reg, d_size, i_size, value))
			{
				return false;
			}

			switch (d_size)
			{
			case 1: value = *(atomic_t<u8>*)vm::base_priv(addr) ^= (u8)value; break;
			case 2: value = *(atomic_t<u16>*)vm::base_priv(addr) ^= (u16)value; break;
			case 4: value = *(atomic_t<u32>*)vm::base_priv(addr) ^= (u32)value; break;
			case 8: value = *(atomic_t<u64>*)vm::base_priv(addr) ^= (u64)value; break;
			default: return false;
			}

			if (!set_x64_cmp_flags(context, d_size, value, 0))
			{
				return false;
			}
			break;
		}
		case X64OP_INC:
		{
			u64 value;

			switch (d_size)
			{
			case 1: value = ++*(atomic_t<u8>*)vm::base_priv(addr); break;
			case 2: value = ++*(atomic_t<u16>*)vm::base_priv(addr); break;
			case 4: value = ++*(atomic_t<u32>*)vm::base_priv(addr); break;
			case 8: value = ++*(atomic_t<u64>*)vm::base_priv(addr); break;
			default: return false;
			}

			if (!set_x64_cmp_flags(context, d_size, value, 1, false)) // ???
			{
				return false;
			}
			break;
		}
		case X64OP_DEC:
		{
			u64 value;

			switch (d_size)
			{
			case 1: value = --*(atomic_t<u8>*)vm::base_priv(addr); break;
			case 2: value = --*(atomic_t<u16>*)vm::base_priv(addr); break;
			case 4: value = --*(atomic_t<u32>*)vm::base_priv(addr); break;
			case 8: value = --*(atomic_t<u64>*)vm::base_priv(addr); break;
			default: return false;
			}

			if (!set_x64_cmp_flags(context, d_size, value, -1, false)) // ???
			{
				return false;
			}
			break;
		}
		case X64OP_ADD:
		{
			u64 value, new_value;
			if (!get_x64_reg_value(context, reg, d_size, i_size, value))
			{
				return false;
			}

			switch (d_size)
			{
			case 1: new_value = *(atomic_t<u8>*)vm::base_priv(addr) += (u8)value; break;
			case 2: new_value = *(atomic_t<u16>*)vm::base_priv(addr) += (u16)value; break;
			case 4: new_value = *(atomic_t<u32>*)vm::base_priv(addr) += (u32)value; break;
			case 8: new_value = *(atomic_t<u64>*)vm::base_priv(addr) += (u64)value; break;
			default: return false;
			}

			if (!set_x64_cmp_flags(context, d_size, new_value, value)) // ???
			{
				return false;
			}
			break;
		}
		case X64OP_ADC:
		{
			u64 value, new_value;
			if (!get_x64_reg_value(context, reg, d_size, i_size, value))
			{
				return false;
			}

			switch (d_size)
			{
			case 1: new_value = *(atomic_t<u8>*)vm::base_priv(addr) += (u8)(value + (EFLAGS(context) & 1)); break;
			case 2: new_value = *(atomic_t<u16>*)vm::base_priv(addr) += (u16)(value + (EFLAGS(context) & 1)); break;
			case 4: new_value = *(atomic_t<u32>*)vm::base_priv(addr) += (u32)(value + (EFLAGS(context) & 1)); break;
			case 8: new_value = *(atomic_t<u64>*)vm::base_priv(addr) += (u64)(value + (EFLAGS(context) & 1)); break;
			default: return false;
			}

			if (!set_x64_cmp_flags(context, d_size, new_value, value + (EFLAGS(context) & 1))) // ???
			{
				return false;
			}
			break;
		}
		case X64OP_SUB:
		{
			u64 value, new_value;
			if (!get_x64_reg_value(context, reg, d_size, i_size, value))
			{
				return false;
			}

			switch (d_size)
			{
			case 1: new_value = *(atomic_t<u8>*)vm::base_priv(addr) -= (u8)value; break;
			case 2: new_value = *(atomic_t<u16>*)vm::base_priv(addr) -= (u16)value; break;
			case 4: new_value = *(atomic_t<u32>*)vm::base_priv(addr) -= (u32)value; break;
			case 8: new_value = *(atomic_t<u64>*)vm::base_priv(addr) -= (u64)value; break;
			default: return false;
			}

			if (!set_x64_cmp_flags(context, d_size, new_value, 0 - value)) // ???
			{
				return false;
			}
			break;
		}
		case X64OP_SBB:
		{
			u64 value, new_value;
			if (!get_x64_reg_value(context, reg, d_size, i_size, value))
			{
				return false;
			}

			switch (d_size)
			{
			case 1: new_value = *(atomic_t<u8>*)vm::base_priv(addr) -= (u8)(value + (EFLAGS(context) & 1)); break;
			case 2: new_value = *(atomic_t<u16>*)vm::base_priv(addr) -= (u16)(value + (EFLAGS(context) & 1)); break;
			case 4: new_value = *(atomic_t<u32>*)vm::base_priv(addr) -= (u32)(value + (EFLAGS(context) & 1)); break;
			case 8: new_value = *(atomic_t<u64>*)vm::base_priv(addr) -= (u64)(value + (EFLAGS(context) & 1)); break;
			default: return false;
			}

			if (!set_x64_cmp_flags(context, d_size, new_value, 0 - (value + (EFLAGS(context) & 1)))) // ???
			{
				return false;
			}
			break;
		}
		default:
		{
			LOG_ERROR(MEMORY, "Invalid or unsupported operation (op=%d, reg=%d, d_size=%lld, a_size=0x%llx, i_size=%lld)", (u32)op, (u32)reg, d_size, a_size, i_size);
			report_opcode();
			return false;
		}
		}

		// skip processed instruction
		RIP(context) += i_size;
		return true;
	});

	// TODO: allow recovering from a page fault as a feature of PS3 virtual memory
}

// Detect leaf function
static bool is_leaf_function(u64 rip)
{
#ifdef _WIN32
	DWORD64 base = 0;
	if (const auto rtf = RtlLookupFunctionEntry(rip, &base, nullptr))
	{
		// Access UNWIND_INFO structure
		const auto uw = (u8*)(base + rtf->UnwindData);

		// Leaf function has zero epilog size and no unwind codes
		return uw[0] == 1 && uw[1] == 0 && uw[2] == 0 && uw[3] == 0;
	}

	// No unwind info implies leaf function
	return true;
#else
	// TODO
	return false;
#endif
}

static thread_local u64 s_tls_ret_pos = 0;
static thread_local u64 s_tls_ret_addr = 0;

[[noreturn]] static void throw_access_violation(const char* cause, u64 addr)
{
	if (s_tls_ret_pos) *(u64*)s_tls_ret_pos = s_tls_ret_addr; // Fix stack
	vm::throw_access_violation(addr, cause);
	std::abort();
}

// Modify context in order to convert hardware exception to C++ exception
static void prepare_throw_access_violation(x64_context* context, const char* cause, u32 address)
{
	// Set throw_access_violation() call args (old register values are lost)
	ARG1(context) = (u64)cause;
	ARG2(context) = address;

	// Push the exception address as a "return" address (throw_access_violation() shall not return)
	s_tls_ret_addr = RIP(context);
	s_tls_ret_pos = is_leaf_function(s_tls_ret_addr) ? 0 : RSP(context) -= sizeof(u64);
	RIP(context) = (u64)std::addressof(throw_access_violation);
}

static void _handle_interrupt(x64_context* ctx);

#ifdef _WIN32

static LONG exception_handler(PEXCEPTION_POINTERS pExp)
{
	const u64 addr64 = pExp->ExceptionRecord->ExceptionInformation[1] - (u64)vm::base(0);
	const bool is_writing = pExp->ExceptionRecord->ExceptionInformation[0] != 0;

	if (pExp->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && addr64 < 0x100000000ull)
	{
		vm::g_tls_fault_count++;

		if (thread_ctrl::get_current() && handle_access_violation((u32)addr64, is_writing, pExp->ContextRecord))
		{
			return EXCEPTION_CONTINUE_EXECUTION;
		}
	}

	return EXCEPTION_CONTINUE_SEARCH;
}

static LONG exception_filter(PEXCEPTION_POINTERS pExp)
{
	std::string msg = fmt::format("Unhandled Win32 exception 0x%08X.\n", pExp->ExceptionRecord->ExceptionCode);

	if (pExp->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION)
	{
		const u64 addr64 = pExp->ExceptionRecord->ExceptionInformation[1] - (u64)vm::base(0);
		const auto cause = pExp->ExceptionRecord->ExceptionInformation[0] != 0 ? "writing" : "reading";

		if (!(vm::g_tls_fault_count & (1ull << 63)) && addr64 < 0x100000000ull)
		{
			vm::g_tls_fault_count |= (1ull << 63);
			// Setup throw_access_violation() call on the context
			prepare_throw_access_violation(pExp->ContextRecord, cause, (u32)addr64);
			return EXCEPTION_CONTINUE_EXECUTION;
		}

		msg += fmt::format("Segfault %s location %p at %p.\n", cause, pExp->ExceptionRecord->ExceptionInformation[1], pExp->ExceptionRecord->ExceptionAddress);
	}
	else
	{
		msg += fmt::format("Exception address: %p.\n", pExp->ExceptionRecord->ExceptionAddress);

		for (DWORD i = 0; i < pExp->ExceptionRecord->NumberParameters; i++)
		{
			msg += fmt::format("ExceptionInformation[0x%x]: %p.\n", i, pExp->ExceptionRecord->ExceptionInformation[i]);
		}
	}

	std::vector<HMODULE> modules;
	for (DWORD size = 256; modules.size() != size; size /= sizeof(HMODULE))
	{
		modules.resize(size);
		if (!EnumProcessModules(GetCurrentProcess(), modules.data(), size * sizeof(HMODULE), &size))
		{
			modules.clear();
			break;
		}
	}

	msg += fmt::format("Instruction address: %p.\n", pExp->ContextRecord->Rip);

	DWORD64 unwind_base;
	if (const auto rtf = RtlLookupFunctionEntry(pExp->ContextRecord->Rip, &unwind_base, nullptr))
	{
		// Get function address
		const DWORD64 func_addr = rtf->BeginAddress + unwind_base;
		msg += fmt::format("Function address: %p (base+0x%x).\n", func_addr, rtf->BeginAddress);

		// Access UNWIND_INFO structure
		//const auto uw = (u8*)(unwind_base + rtf->UnwindData);
	}

	for (HMODULE module : modules)
	{
		MODULEINFO info;
		if (GetModuleInformation(GetCurrentProcess(), module, &info, sizeof(info)))
		{
			const DWORD64 base = (DWORD64)info.lpBaseOfDll;

			if (pExp->ContextRecord->Rip >= base && pExp->ContextRecord->Rip < base + info.SizeOfImage)
			{
				std::string module_name;
				for (DWORD size = 15; module_name.size() != size;)
				{
					module_name.resize(size);
					size = GetModuleBaseNameA(GetCurrentProcess(), module, &module_name.front(), size + 1);
					if (!size)
					{
						module_name.clear();
						break;
					}
				}

				msg += fmt::format("Module name: '%s'.\n", module_name);
				msg += fmt::format("Module base: %p.\n", info.lpBaseOfDll);
			}
		}
	}

	msg += fmt::format("RPCS3 image base: %p.\n", GetModuleHandle(NULL));

	if (pExp->ExceptionRecord->ExceptionCode == EXCEPTION_ILLEGAL_INSTRUCTION)
	{
		msg += "\n"
			"Illegal instruction exception occured.\n"
			"Note that your CPU must support SSSE3 extension.\n";
	}

	// TODO: print registers and the callstack

	// Report fatal error
	report_fatal_error(msg);
	return EXCEPTION_CONTINUE_SEARCH;
}

const bool s_exception_handler_set = []() -> bool
{
	if (!AddVectoredExceptionHandler(1, (PVECTORED_EXCEPTION_HANDLER)exception_handler))
	{
		report_fatal_error("AddVectoredExceptionHandler() failed.");
		std::abort();
	}

	if (!SetUnhandledExceptionFilter((LPTOP_LEVEL_EXCEPTION_FILTER)exception_filter))
	{
		report_fatal_error("SetUnhandledExceptionFilter() failed.");
		std::abort();
	}

	return true;
}();

#else

static void signal_handler(int sig, siginfo_t* info, void* uct)
{
	x64_context* context = (ucontext_t*)uct;

	if (sig == SIGUSR1)
	{
		return _handle_interrupt(context);
	}

#ifdef __APPLE__
	const bool is_writing = context->uc_mcontext->__es.__err & 0x2;
#else
	const bool is_writing = context->uc_mcontext.gregs[REG_ERR] & 0x2;
#endif

	const u64 addr64 = (u64)info->si_addr - (u64)vm::base(0);
	const auto cause = is_writing ? "writing" : "reading";

	if (addr64 < 0x100000000ull)
	{
		vm::g_tls_fault_count++;

		// Try to process access violation
		if (!thread_ctrl::get_current() || !handle_access_violation((u32)addr64, is_writing, context))
		{
			// Setup throw_access_violation() call on the context
			prepare_throw_access_violation(context, cause, (u32)addr64);
		}
	}
	else
	{
		// TODO (debugger interaction)
		report_fatal_error(fmt::format("Segfault %s location %p at %p.", cause, info->si_addr, RIP(context)));
		std::abort();
	}
}

const bool s_exception_handler_set = []() -> bool
{
	struct ::sigaction sa;
	sa.sa_flags = SA_SIGINFO;
	sigemptyset(&sa.sa_mask);
	sa.sa_sigaction = signal_handler;

	if (::sigaction(SIGSEGV, &sa, NULL) == -1)
	{
		std::printf("sigaction(SIGSEGV) failed (0x%x).", errno);
		std::abort();
	}

	sa.sa_sigaction = signal_handler;

	if (::sigaction(SIGUSR1, &sa, NULL) == -1)
	{
		std::printf("sigaction(SIGUSR1) failed (0x%x).", errno);
		std::abort();
	}

	return true;
}();

#endif

const bool s_self_test = []() -> bool
{
	// Find ret instruction
	if ((*(u8*)throw_access_violation & 0xF6) == 0xC2)
	{
		std::abort();
	}

	return true;
}();

#include <thread>
#include <mutex>
#include <condition_variable>
#include <exception>
#include <chrono>

thread_local DECLARE(thread_ctrl::g_tls_this_thread) = nullptr;

struct thread_ctrl::internal
{
	std::mutex mutex;
	std::condition_variable cond;
	std::condition_variable jcv; // Allows simultaneous joining
	std::condition_variable icv;

	task_stack atexit;

	std::exception_ptr exception; // Stored exception

	std::chrono::high_resolution_clock::time_point time_limit;

#ifdef _WIN32
	DWORD thread_id = 0;
#endif

	x64_context _context{};
	x64_context* const thread_ctx = &this->_context;

	atomic_t<void(*)()> interrupt{}; // Interrupt function
};

thread_local thread_ctrl::internal* g_tls_internal = nullptr;

extern std::condition_variable& get_current_thread_cv()
{
	return g_tls_internal->cond;
}

// TODO
extern atomic_t<u32> g_thread_count(0);

extern thread_local std::string(*g_tls_log_prefix)();

void thread_ctrl::start(const std::shared_ptr<thread_ctrl>& ctrl, task_stack task)
{
	reinterpret_cast<std::thread&>(ctrl->m_thread) = std::thread([ctrl, task = std::move(task)]
	{
		try
		{
			ctrl->initialize();
			task.exec();
		}
		catch (...)
		{
			ctrl->m_data->exception = std::current_exception();
		}

		ctrl->finalize();
	});
}

void thread_ctrl::wait_start(u64 timeout)
{
	m_data->time_limit = std::chrono::high_resolution_clock::now() + std::chrono::microseconds(timeout);
}

bool thread_ctrl::wait_wait(u64 timeout)
{
	std::unique_lock<std::mutex> lock(m_data->mutex, std::adopt_lock);

	if (timeout && m_data->cond.wait_until(lock, m_data->time_limit) == std::cv_status::timeout)
	{
		lock.release();
		return false;
	}

	m_data->cond.wait(lock);
	lock.release();
	return true;
}

void thread_ctrl::test()
{
	if (m_data && m_data->exception)
	{
		std::rethrow_exception(m_data->exception);
	}
}

void thread_ctrl::initialize()
{
	// Initialize TLS variable
	g_tls_this_thread = this;
	g_tls_internal = this->m_data;
#ifdef _WIN32
	m_data->thread_id = GetCurrentThreadId();
#endif

	g_tls_log_prefix = []
	{
		return g_tls_this_thread->m_name;
	};

	++g_thread_count;

#if defined(_MSC_VER)

	struct THREADNAME_INFO
	{
		DWORD dwType;
		LPCSTR szName;
		DWORD dwThreadID;
		DWORD dwFlags;
	};

	// Set thread name for VS debugger
	if (IsDebuggerPresent())
	{
		THREADNAME_INFO info;
		info.dwType = 0x1000;
		info.szName = m_name.c_str();
		info.dwThreadID = -1;
		info.dwFlags = 0;

		__try
		{
			RaiseException(0x406D1388, 0, sizeof(info) / sizeof(ULONG_PTR), (ULONG_PTR*)&info);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
		}
	}

#endif
}

void thread_ctrl::finalize() noexcept
{
	// Disable and discard possible interrupts
	interrupt_disable();
	test_interrupt();

	// TODO
	vm::reservation_free();

	// Call atexit functions
	if (m_data) m_data->atexit.exec();

	--g_thread_count;

#ifdef _WIN32
	ULONG64 time;
	QueryThreadCycleTime(GetCurrentThread(), &time);
	LOG_NOTICE(GENERAL, "Thread time: %f Gc", time / 1000000000.);
#endif
}

void thread_ctrl::push_atexit(task_stack task)
{
	m_data->atexit.push(std::move(task));
}

thread_ctrl::thread_ctrl(std::string&& name)
	: m_name(std::move(name))
{
	CHECK_STORAGE(std::thread, m_thread);

#pragma push_macro("new")
#undef new
	new (&m_thread) std::thread;
#pragma pop_macro("new")

	initialize_once();
}

thread_ctrl::~thread_ctrl()
{
	if (reinterpret_cast<std::thread&>(m_thread).joinable())
	{
		reinterpret_cast<std::thread&>(m_thread).detach();
	}

	delete m_data;

	reinterpret_cast<std::thread&>(m_thread).~thread();
}

void thread_ctrl::initialize_once()
{
	if (UNLIKELY(!m_data))
	{
		auto ptr = new thread_ctrl::internal;

		if (!m_data.compare_and_swap_test(nullptr, ptr))
		{
			delete ptr;
		}
	}
}

void thread_ctrl::join()
{
	// Increase contention counter
	const u32 _j = m_joining++;

	if (LIKELY(_j >= 0x80000000))
	{
		// Already joined (signal condition)
		m_joining = 0x80000000;
	}
	else if (LIKELY(_j == 0))
	{
		// Winner joins the thread
		reinterpret_cast<std::thread&>(m_thread).join();

		// Notify others if necessary
		if (UNLIKELY(m_joining.exchange(0x80000000) != 1))
		{
			// Serialize for reliable notification
			m_data->mutex.lock();
			m_data->mutex.unlock();
			m_data->jcv.notify_all();
		}
	}
	else
	{
		// Hard way
		std::unique_lock<std::mutex> lock(m_data->mutex);
		m_data->jcv.wait(lock, WRAP_EXPR(m_joining >= 0x80000000));
	}

	if (UNLIKELY(m_data && m_data->exception && !std::uncaught_exception()))
	{
		std::rethrow_exception(m_data->exception);
	}
}

void thread_ctrl::lock()
{
	m_data->mutex.lock();
}

void thread_ctrl::unlock()
{
	m_data->mutex.unlock();
}

void thread_ctrl::lock_notify()
{
	if (UNLIKELY(g_tls_this_thread == this))
	{
		return;
	}

	// Serialize for reliable notification, condition is assumed to be changed externally
	m_data->mutex.lock();
	m_data->mutex.unlock();
	m_data->cond.notify_one();
}

void thread_ctrl::notify()
{
	m_data->cond.notify_one();
}

void thread_ctrl::set_exception(std::exception_ptr e)
{
	m_data->exception = e;
}

static void _handle_interrupt(x64_context* ctx)
{
	// Copy context for further use (TODO: is it safe on all platforms?)
	g_tls_internal->_context = *ctx;
	thread_ctrl::handle_interrupt();
}

static thread_local void(*s_tls_handler)() = nullptr;

[[noreturn]] static void execute_interrupt_handler()
{
	// Fix stack for throwing
	if (s_tls_ret_pos) s_tls_ret_addr = std::exchange(*(u64*)s_tls_ret_pos, s_tls_ret_addr);

	// Run either throwing or returning interrupt handler
	s_tls_handler();

	// Restore context in the case of return
	const auto ctx = g_tls_internal->thread_ctx;

	if (s_tls_ret_pos)
	{
		RIP(ctx) = std::exchange(*(u64*)s_tls_ret_pos, s_tls_ret_addr);
		RSP(ctx) += sizeof(u64);
	}
	else
	{
		RIP(ctx) = s_tls_ret_addr;
	}

#ifdef _WIN32
	RtlRestoreContext(ctx, nullptr);
#else
	::setcontext(ctx);
#endif
}

void thread_ctrl::handle_interrupt()
{
	const auto _this = g_tls_this_thread;
	const auto ctx = g_tls_internal->thread_ctx;

	if (_this->m_guard & 0x80000000)
	{
		// Discard interrupt if interrupts are disabled
		if (g_tls_internal->interrupt.exchange(nullptr))
		{
			_this->lock();
			_this->unlock();
			g_tls_internal->icv.notify_one();
		}
	}
	else if (_this->m_guard == 0)
	{
		// Set interrupt immediately if no guard set
		if (const auto handler = g_tls_internal->interrupt.exchange(nullptr))
		{
			_this->lock();
			_this->unlock();
			g_tls_internal->icv.notify_one();

#ifdef _WIN32
			// Install function call
			s_tls_ret_addr = RIP(ctx);
			s_tls_ret_pos = is_leaf_function(s_tls_ret_addr) ? 0 : RSP(ctx) -= sizeof(u64);
			s_tls_handler = handler;
			RIP(ctx) = (u64)execute_interrupt_handler;
#else
			// Call handler directly (TODO: install function call preserving red zone)
			return handler();
#endif
		}
	}
	else
	{
		// Set delayed interrupt otherwise
		_this->m_guard |= 0x40000000;
	}
	
#ifdef _WIN32
	RtlRestoreContext(ctx, nullptr);
#endif
}

void thread_ctrl::interrupt(void(*handler)())
{
	VERIFY(this != g_tls_this_thread); // TODO: self-interrupt
	VERIFY(m_data->interrupt.compare_and_swap_test(nullptr, handler)); // TODO: multiple interrupts

#ifdef _WIN32
	const auto ctx = m_data->thread_ctx;

	const HANDLE nt = OpenThread(THREAD_ALL_ACCESS, FALSE, m_data->thread_id);
	VERIFY(nt);
	VERIFY(SuspendThread(nt) != -1);

	ctx->ContextFlags = CONTEXT_FULL;
	VERIFY(GetThreadContext(nt, ctx));

	ctx->ContextFlags = CONTEXT_FULL;
	const u64 _rip = RIP(ctx);
	RIP(ctx) = (u64)std::addressof(thread_ctrl::handle_interrupt);
	VERIFY(SetThreadContext(nt, ctx));

	RIP(ctx) = _rip;
	VERIFY(ResumeThread(nt) != -1);
	CloseHandle(nt);
#else
	pthread_kill(reinterpret_cast<std::thread&>(m_thread).native_handle(), SIGUSR1);
#endif

	std::unique_lock<std::mutex> lock(m_data->mutex, std::adopt_lock);

	while (m_data->interrupt)
	{
		m_data->icv.wait(lock);
	}

	lock.release();
}

void thread_ctrl::test_interrupt()
{
	if (m_guard & 0x80000000)
	{
		if (m_data->interrupt.exchange(nullptr))
		{
			lock(), unlock(), m_data->icv.notify_one();
		}
		
		return;
	}

	if (m_guard == 0x40000000 && !std::uncaught_exception())
	{
		m_guard = 0;

		// Execute delayed interrupt handler
		if (const auto handler = m_data->interrupt.exchange(nullptr))
		{
			lock(), unlock(), m_data->icv.notify_one();

			return handler();
		}
	}
}

void thread_ctrl::sleep(u64 useconds)
{
	std::this_thread::sleep_for(std::chrono::microseconds(useconds));
}


named_thread::named_thread()
{
}

named_thread::~named_thread()
{
}

std::string named_thread::get_name() const
{
	return fmt::format("('%s') Unnamed Thread", typeid(*this).name());
}

void named_thread::start_thread(const std::shared_ptr<void>& _this)
{
	// Ensure it's not called from the constructor and the correct object is passed
	ENSURES(_this.get() == this);

	// Run thread
	thread_ctrl::spawn(m_thread, get_name(), [this, _this]()
	{
		try
		{
			LOG_TRACE(GENERAL, "Thread started");
			on_task();
			LOG_TRACE(GENERAL, "Thread ended");
		}
		catch (const std::exception& e)
		{
			LOG_FATAL(GENERAL, "%s thrown: %s", typeid(e).name(), e.what());
			Emu.Pause();
		}
		catch (EmulationStopped)
		{
			LOG_NOTICE(GENERAL, "Thread aborted");
		}

		on_exit();
	});
}
