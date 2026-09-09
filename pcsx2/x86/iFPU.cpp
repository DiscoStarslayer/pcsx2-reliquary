// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Common.h"
#include "R5900OpcodeTables.h"
#include "iR5900.h"
#include "microVU_SoftFloatTables.h"
#include "SoftFloatEmitter.h"

using namespace x86Emitter;

alignas(16) const u32 g_minvals[4] = {0xff7fffff, 0xff7fffff, 0xff7fffff, 0xff7fffff};
alignas(16) const u32 g_maxvals[4] = {0x7f7fffff, 0x7f7fffff, 0x7f7fffff, 0x7f7fffff};

//------------------------------------------------------------------
namespace R5900 {
namespace Dynarec {
namespace OpcodeImpl {
namespace COP1 {

namespace DOUBLE
{

	void recABS_S_xmm(int info);
	void recADD_S_xmm(int info);
	void recADDA_S_xmm(int info);
	void recC_EQ_xmm(int info);
	void recC_LE_xmm(int info);
	void recC_LT_xmm(int info);
	void recDIV_S_xmm(int info);
	void recMADD_S_xmm(int info);
	void recMADDA_S_xmm(int info);
	void recMAX_S_xmm(int info);
	void recMIN_S_xmm(int info);
	void recMOV_S_xmm(int info);
	void recMSUB_S_xmm(int info);
	void recMSUBA_S_xmm(int info);
	void recMUL_S_xmm(int info);
	void recMULA_S_xmm(int info);
	void recNEG_S_xmm(int info);
	void recSUB_S_xmm(int info);
	void recSUBA_S_xmm(int info);
	void recSQRT_S_xmm(int info);
	void recRSQRT_S_xmm(int info);

}; // namespace DOUBLE

//------------------------------------------------------------------
// Helper Macros
//------------------------------------------------------------------
#define _Ft_ _Rt_
#define _Fs_ _Rd_
#define _Fd_ _Sa_

// FCR31 Flags
#define FPUflagC  0x00800000
#define FPUflagI  0x00020000
#define FPUflagD  0x00010000
#define FPUflagO  0x00008000
#define FPUflagU  0x00004000
#define FPUflagSI 0x00000040
#define FPUflagSD 0x00000020
#define FPUflagSO 0x00000010
#define FPUflagSU 0x00000008

// Add/Sub opcodes produce the same results as the ps2
#define FPU_CORRECT_ADD_SUB 1

alignas(16) static const u32 s_neg[4] = {0x80000000, 0xffffffff, 0xffffffff, 0xffffffff};
alignas(16) static const u32 s_pos[4] = {0x7fffffff, 0xffffffff, 0xffffffff, 0xffffffff};

#define REC_FPUBRANCH(f) \
	void f(); \
	void rec##f() \
	{ \
		iFlushCall(FLUSH_INTERPRETER); \
		xFastCall((void*)(uptr)R5900::Interpreter::OpcodeImpl::COP1::f); \
		g_branch = 2; \
	}

#define REC_FPUFUNC(f) \
	void f(); \
	void rec##f() \
	{ \
		iFlushCall(FLUSH_INTERPRETER); \
		xFastCall((void*)(uptr)R5900::Interpreter::OpcodeImpl::COP1::f); \
	}

#define FPURECOMPILE_CONSTCODE_EXACT(fn, xmminfo, ...) \
	void rec##fn(void) \
	{ \
		if (CHECK_FPU_SOFT) \
			eeFPURecompileCode(__VA_ARGS__, R5900::Interpreter::OpcodeImpl::COP1::fn, xmminfo); \
		else if (CHECK_FPU_FULL) \
			eeFPURecompileCode(DOUBLE::rec##fn##_xmm, R5900::Interpreter::OpcodeImpl::COP1::fn, xmminfo); \
		else \
			eeFPURecompileCode(rec##fn##_xmm, R5900::Interpreter::OpcodeImpl::COP1::fn, xmminfo); \
	}

static const void* s_fpuSoftAddSubExact[2];
static const void* s_fpuSoftMulExact;
static const void* s_fpuSoftMaddExact[2];
static const void* s_fpuSoftDivExact;
static const void* s_fpuSoftDivCapExact;
static const void* s_fpuSoftSqrtExact;
static const void* s_fpuSoftRsqrtExact;
alignas(16) static FPControlRegister s_fpuSoftSqrtChopMode;


void GenerateSoftFloatKernels()
{
	constexpr int result_overflow = 1;
	constexpr int result_underflow = 2;
	if (!CHECK_FPU_SOFT)
		return;

	MicroVUSoftFloatTables::InitializeCorrectionTables();

	{
		for (int subtract = 0; subtract < 2; subtract++)
		{
			// Internal ABI: eax = first raw operand, edx = second raw operand.
			// Returns eax = raw result and edx = overflow/underflow flags.
			s_fpuSoftAddSubExact[subtract] = xGetAlignedCallTarget();
			xMOV(r9d, eax);
			xMOV(r10d, edx);
			xXOR(r11d, r11d);
			if (subtract)
				xXOR(r10d, 0x80000000);

			xMOV(ecx, r9d);
			xSHR(ecx, 23);
			xAND(ecx, 0xff);
			xMOV(edx, r10d);
			xSHR(edx, 23);
			xAND(edx, 0xff);
			xTEST(ecx, ecx);
			xForwardJZ32 add_self_denormal;
			xTEST(edx, edx);
			xForwardJZ32 add_other_denormal;

			xSUB(ecx, edx);
			xCMP(ecx, 25);
			xForwardJGE32 add_result_self_large_diff;
			xCMP(ecx, -25);
			xForwardJLE32 add_result_other_large_diff;
			xCMP(ecx, 0);
			xForwardJG32 add_truncate_other;
			xForwardJZ32 add_operands_ready;

			xNEG(ecx);
			xDEC(ecx);
			xMOV(edx, 0xffffffff);
			xSHL(edx, cl);
			xINC(ecx);
			xAND(r9d, edx);
			xMOV(eax, r9d);
			xMOV(r9d, r10d);
			xMOV(r10d, eax);
			xForwardJump32 add_operands_ready_from_other;

			add_truncate_other.SetTarget();
			xDEC(ecx);
			xMOV(edx, 0xffffffff);
			xSHL(edx, cl);
			xINC(ecx);
			xAND(r10d, edx);

			add_operands_ready.SetTarget();
			add_operands_ready_from_other.SetTarget();
			xMOV(eax, r9d);
			xMOV(edx, eax);
			xSHR(edx, 23);
			xAND(edx, 0xff);
			xMOV(r11d, edx);

			xMOV(eax, r10d);
			xMOV(edx, eax);
			xSAR(edx, 31);
			xAND(eax, 0x7fffff);
			xOR(eax, 0x800000);
			xXOR(eax, edx);
			xSUB(eax, edx);
			xSHL(eax, 6);
			xSAR(eax, cl);
			xMOV(r10d, eax);

			xMOV(eax, r9d);
			xMOV(edx, eax);
			xSAR(edx, 31);
			xAND(eax, 0x7fffff);
			xOR(eax, 0x800000);
			xXOR(eax, edx);
			xSUB(eax, edx);
			xSHL(eax, 6);
			xADD(eax, r10d);
			xMOV(edx, eax);
			xAND(edx, 0x80000000);
			xMOV(r9d, edx);
			xMOV(edx, eax);
			xSAR(edx, 31);
			xXOR(eax, edx);
			xSUB(eax, edx);
			xForwardJZ32 add_result_zero;

			xBSR(ecx, eax);
			xADD(r11d, ecx);
			xSUB(r11d, 29);
			xCMP(ecx, 23);
			xForwardJLE32 add_normalize_left;
			xSUB(ecx, 23);
			xSHR(eax, cl);
			xForwardJump32 add_normalized;

			add_normalize_left.SetTarget();
			xNEG(ecx);
			xADD(ecx, 23);
			xSHL(eax, cl);

			add_normalized.SetTarget();
			xAND(eax, 0x7fffff);
			xMOV(edx, r11d);
			xCMP(edx, 255);
			xForwardJG32 add_overflow_result;
			xCMP(edx, 1);
			xForwardJL32 add_underflow_result;
			xSHL(edx, 23);
			xOR(eax, edx);
			xOR(eax, r9d);
			xXOR(r11d, r11d);
			xForwardJump32 add_result_ready;

			add_overflow_result.SetTarget();
			xMOV(eax, r9d);
			xOR(eax, 0x7fffffff);
			xMOV(r11d, result_overflow);
			xForwardJump32 add_result_ready_from_overflow;

			add_underflow_result.SetTarget();
			xOR(eax, r9d);
			xMOV(r11d, result_underflow);
			xForwardJump32 add_result_ready_from_underflow;

			add_result_zero.SetTarget();
			xXOR(eax, eax);
			xXOR(r11d, r11d);
			xForwardJump32 add_result_ready_from_zero;

			add_self_denormal.SetTarget();
			xTEST(edx, edx);
			xForwardJZ32 add_both_denormal;
			xMOV(eax, r10d);
			xForwardJump32 add_result_ready_from_self_denormal;

			add_both_denormal.SetTarget();
			xMOV(eax, r9d);
			xAND(eax, 0x80000000);
			xAND(eax, r10d);
			xForwardJump32 add_result_ready_from_both_denormal;

			add_other_denormal.SetTarget();
			add_result_self_large_diff.SetTarget();
			xMOV(eax, r9d);
			xForwardJump32 add_result_ready_from_self;

			add_result_other_large_diff.SetTarget();
			xMOV(eax, r10d);

			add_result_ready.SetTarget();
			add_result_ready_from_overflow.SetTarget();
			add_result_ready_from_underflow.SetTarget();
			add_result_ready_from_zero.SetTarget();
			add_result_ready_from_self_denormal.SetTarget();
			add_result_ready_from_both_denormal.SetTarget();
			add_result_ready_from_self.SetTarget();
			xMOV(edx, r11d);
			xRET();
		}

		// Internal ABI: eax = first raw operand, edx = second raw operand.
		// Returns eax = raw product and edx = overflow/underflow flags.
		// The flag bits match the ADD/SUB kernels: bit 0 overflow, bit 1 underflow.
		constexpr sptr fs_raw = 0;
		constexpr int operand_raw = fs_raw + 4;
		constexpr int product_raw = operand_raw + 4;
		constexpr int product_flags = product_raw + 4;
		constexpr int product_exponent = product_flags + 4;
		constexpr int mantissa_a = product_exponent + 4;
		constexpr int full_lo = mantissa_a + 4;
		constexpr int full_hi = full_lo + 4;
		constexpr int booth_negate = full_hi + 4;
		constexpr int booth_data = booth_negate + 8 * 4;
		constexpr int add3_values = booth_data + 8 * 4;
		constexpr int mul_stack_size = (add3_values + 12 * 4 + 15) & ~15;
		constexpr int product_underflow = 2;
		constexpr int product_overflow = 1;
		constexpr int t0_lo = add3_values + 0 * 4;
		constexpr int t0_hi = add3_values + 1 * 4;
		constexpr int t1_lo = add3_values + 2 * 4;
		constexpr int t1_hi = add3_values + 3 * 4;
		constexpr int t2_lo = add3_values + 4 * 4;
		constexpr int t2_hi = add3_values + 5 * 4;
		constexpr int t3_lo = add3_values + 6 * 4;
		constexpr int t3_hi = add3_values + 7 * 4;
		constexpr int t4_lo = add3_values + 8 * 4;
		constexpr int t4_hi = add3_values + 9 * 4;
		constexpr int t5_lo = add3_values + 10 * 4;
		constexpr int t5_hi = add3_values + 11 * 4;

		s_fpuSoftMulExact = xGetAlignedCallTarget();
		xSUB(rsp, mul_stack_size);
		xMOV(ptr32[rsp + fs_raw], eax);
		xMOV(ptr32[rsp + operand_raw], edx);
		xXOR(eax, edx);
		xAND(eax, 0x80000000);
		xMOV(ptr32[rsp + product_raw], eax);
		xMOV(ptr32[rsp + product_flags], 0);

		xMOV(eax, ptr32[rsp + fs_raw]);
		xSHR(eax, 23);
		xAND(eax, 0xff);
		xForwardJZ32 product_zero_from_fs;
		xMOV(edx, ptr32[rsp + operand_raw]);
		xSHR(edx, 23);
		xAND(edx, 0xff);
		xForwardJZ32 product_zero_from_operand;
		xADD(eax, edx);
		xSUB(eax, 127);
		xMOV(ptr32[rsp + product_exponent], eax);
		xCMP(eax, 0);
		xForwardJL32 product_underflow_result;
		xCMP(eax, 255);
		xForwardJG32 product_overflow_result;
		xMOV(edx, ptr32[rsp + operand_raw]);
		xTEST(edx, 0x7fffff);
		xForwardJNZ8 product_requires_booth;
		xCMP(eax, 1);
		xForwardJL32 product_underflow_power_operand;
		xMOV(edx, ptr32[rsp + fs_raw]);
		xAND(edx, 0x7fffff);
		xSHL(eax, 23);
		xOR(eax, edx);
		xOR(eax, ptr32[rsp + product_raw]);
		xMOV(ptr32[rsp + product_raw], eax);
		xForwardJump32 product_ready_from_power_operand;

		product_requires_booth.SetTarget();
		xMOV(eax, ptr32[rsp + fs_raw]);
		xAND(eax, 0x7fffff);
		xOR(eax, 0x800000);
		xMOV(ptr32[rsp + mantissa_a], eax);
		xMOV(ecx, ptr32[rsp + operand_raw]);
		xAND(ecx, 0x7fffff);
		xOR(ecx, 0x800000);
		xMOV(r9d, ecx);
		xUMUL(ecx);
		xMOV(ptr32[rsp + full_lo], eax);
		xMOV(ptr32[rsp + full_hi], edx);
		xAND(eax, 0x7fffff);
		xCMP(eax, 0x8000);
		xForwardJAE32 product_correction_not_visible;
		xMOV(ecx, ptr32[rsp + mantissa_a]);
		xTEST(ecx, 0x7fffff);
		xForwardJNZ32 product_requires_regular_booth_correction;
		xMOV(ecx, r9d);
		xAND(ecx, 0xffff);
		xMOV64(r11, reinterpret_cast<uptr>(MicroVUSoftFloatTables::first_one_correction_lookup));
		xMOVZX(ecx, ptr8[xAddressVoid(r11, rcx, 1)]);
		xSHL(ecx, 15);
		xSUB(ptr32[rsp + full_lo], ecx);
		xSBB(ptr32[rsp + full_hi], 0);
		xForwardJump32 product_first_one_correction_ready;

		product_requires_regular_booth_correction.SetTarget();
			xMOV64(r11, reinterpret_cast<uptr>(X86SoftFloatEmitter::ScalarBoothDecode));
		for (int bit = 0; bit < 8; bit++)
		{
			const u32 shift = bit * 2;
			xMOV(edx, ptr32[rsp + mantissa_a]);
			if (shift != 0)
				xSHL(edx, shift);
			xMOV(eax, r9d);
			if (bit == 0)
				xSHL(eax, 1);
			else
				xSHR(eax, shift - 1);
			xAND(eax, 7);
			xMOV(ecx, ptr32[xAddressVoid(r11, rax, 4)]);
			xMOV(r10d, ecx);
			xAND(r10d, 3);
			xMUL(edx, r10d);
			xSHR(ecx, 8);
			if (shift != 0)
				xSHL(ecx, shift);
			xMOV(ptr32[rsp + booth_negate + bit * 4], ecx);
			xNEG(ecx);
			xXOR(edx, ecx);
			xMOV(ptr32[rsp + booth_data + bit * 4], edx);
		}

			X86SoftFloatEmitter::EmitCarrySaveAdd(booth_data + 1 * 4, booth_data + 2 * 4, booth_data + 3 * 4, t0_lo, t0_hi);
		xAND(ptr32[rsp + booth_data + 4 * 4], ~0x7ffu);
		xMOV(eax, ptr32[rsp + booth_data + 5 * 4]);
		xMOV(ptr32[rsp + mantissa_a], eax);
		xAND(ptr32[rsp + booth_data + 5 * 4], ~0xfffu);
			X86SoftFloatEmitter::EmitCarrySaveAdd(booth_data + 4 * 4, booth_data + 5 * 4, booth_data + 6 * 4, t1_lo, t1_hi);
		xMOV(eax, ptr32[rsp + mantissa_a]);
		xAND(eax, 0x800);
		xOR(eax, ptr32[rsp + booth_negate + 6 * 4]);
		xOR(ptr32[rsp + t1_hi], eax);
		xMOV(eax, ptr32[rsp + mantissa_a]);
		xAND(eax, 0x400);
		xADD(eax, ptr32[rsp + booth_negate + 5 * 4]);
		xOR(ptr32[rsp + booth_data + 7 * 4], eax);
			X86SoftFloatEmitter::EmitCarrySaveAdd(booth_data + 0 * 4, t0_lo, t0_hi, t2_lo, t2_hi);
			X86SoftFloatEmitter::EmitCarrySaveAdd(booth_data + 7 * 4, t1_lo, t1_hi, t3_lo, t3_hi);
			X86SoftFloatEmitter::EmitCarrySaveAdd(t2_hi, t3_lo, t3_hi, t4_lo, t4_hi);
			X86SoftFloatEmitter::EmitCarrySaveAdd(t2_lo, t4_lo, t4_hi, t5_lo, t5_hi);
		xMOV(eax, ptr32[rsp + booth_negate + 7 * 4]);
		xADD(ptr32[rsp + t5_hi], eax);
		xAND(ptr32[rsp + t5_lo], ~0x7fffu);
		xAND(ptr32[rsp + t5_hi], ~0x7fffu);
		xMOV(eax, ptr32[rsp + t5_lo]);
		xADD(eax, ptr32[rsp + t5_hi]);
		xXOR(eax, ptr32[rsp + full_lo]);
		xAND(eax, 0x8000);
		xSUB(ptr32[rsp + full_lo], eax);
		xSBB(ptr32[rsp + full_hi], 0);

		product_correction_not_visible.SetTarget();
		product_first_one_correction_ready.SetTarget();
		xMOV(eax, ptr32[rsp + full_lo]);
		xMOV(edx, ptr32[rsp + full_hi]);
		xSHRD(eax, edx, 23);
		xCMP(eax, 0xffffff);
		xForwardJLE8 product_mantissa_normalized;
		xSHR(eax, 1);
		xINC(ptr32[rsp + product_exponent]);
		product_mantissa_normalized.SetTarget();
		xMOV(edx, ptr32[rsp + product_exponent]);
		xCMP(edx, 255);
		xForwardJG32 product_overflow_after_normalize;
		xCMP(edx, 1);
		xForwardJL32 product_underflow_after_normalize;
		xSHL(edx, 23);
		xAND(eax, 0x7fffff);
		xOR(eax, edx);
		xOR(eax, ptr32[rsp + product_raw]);
		xMOV(ptr32[rsp + product_raw], eax);
		xForwardJump32 product_ready;

		product_zero_from_fs.SetTarget();
		product_zero_from_operand.SetTarget();
		xForwardJump32 product_ready_from_zero;
		product_underflow_power_operand.SetTarget();
		product_underflow_result.SetTarget();
		product_underflow_after_normalize.SetTarget();
		xMOV(ptr32[rsp + product_flags], product_underflow);
		xForwardJump32 product_ready_from_underflow;
		product_overflow_result.SetTarget();
		product_overflow_after_normalize.SetTarget();
		xMOV(eax, ptr32[rsp + product_raw]);
		xOR(eax, 0x7fffffff);
		xMOV(ptr32[rsp + product_raw], eax);
		xMOV(ptr32[rsp + product_flags], product_overflow);
		product_ready.SetTarget();
		product_ready_from_power_operand.SetTarget();
		product_ready_from_zero.SetTarget();
		product_ready_from_underflow.SetTarget();
		xMOV(eax, ptr32[rsp + product_raw]);
		xMOV(edx, ptr32[rsp + product_flags]);
		xADD(rsp, mul_stack_size);
		xRET();

		// Internal ABI: eax = accumulator, edx = raw product, ecx = product
		// overflow/underflow flags, r9d = incoming ACC overflow. Returns eax = raw
		// result and edx = final overflow/underflow flags. r8d is preserved for the
		// opcode emitter's copy of the product flags.
		for (int subtract = 0; subtract < 2; subtract++)
		{
			s_fpuSoftMaddExact[subtract] = xGetAlignedCallTarget();
			xMOV(r10d, edx);
			xMOV(r11d, ecx);

			xTEST(r9d, r9d);
			xForwardJZ32 acc_input_not_overflow;
			xTEST(r11d, product_overflow);
			xForwardJNZ32 mac_exception_limit;
			xMOV(r11d, result_overflow);
			xForwardJump32 add_result_ready_from_acc_overflow;

			acc_input_not_overflow.SetTarget();
			xTEST(r11d, product_overflow);
			xForwardJZ32 regular_add;

			mac_exception_limit.SetTarget();
			xMOV(eax, r10d);
			xTEST(eax, 0x80000000);
			xForwardJump8 mac_exception_min(subtract ? Jcc_Zero : Jcc_NotZero);
			xMOV(eax, 0x7fffffff);
			xForwardJump32 mac_exception_value_ready;
			mac_exception_min.SetTarget();
			xMOV(eax, 0xffffffff);
			mac_exception_value_ready.SetTarget();
			xMOV(r11d, result_overflow);
			xForwardJump32 add_result_ready_from_mac_exception;

			regular_add.SetTarget();
			xMOV(r9d, eax);
			xXOR(r11d, r11d);
			if (subtract)
				xXOR(r10d, 0x80000000);
			xMOV(ecx, r9d);
			xSHR(ecx, 23);
			xAND(ecx, 0xff);
			xMOV(edx, r10d);
			xSHR(edx, 23);
			xAND(edx, 0xff);
			xTEST(ecx, ecx);
			xForwardJZ32 add_acc_denormal;
			xTEST(edx, edx);
			xForwardJZ32 add_product_denormal;

			xSUB(ecx, edx);
			xCMP(ecx, 25);
			xForwardJGE32 add_result_acc_large_diff;
			xCMP(ecx, -25);
			xForwardJLE32 add_result_product_large_diff;
			xCMP(ecx, 0);
			xForwardJG32 add_truncate_product;
			xForwardJZ32 add_operands_ready;

			xNEG(ecx);
			xDEC(ecx);
			xMOV(edx, 0xffffffff);
			xSHL(edx, cl);
			xINC(ecx);
			xAND(r9d, edx);
			xMOV(eax, r9d);
			xMOV(r9d, r10d);
			xMOV(r10d, eax);
			xForwardJump32 add_operands_ready_from_product;

			add_truncate_product.SetTarget();
			xDEC(ecx);
			xMOV(edx, 0xffffffff);
			xSHL(edx, cl);
			xINC(ecx);
			xAND(r10d, edx);

			add_operands_ready.SetTarget();
			add_operands_ready_from_product.SetTarget();
			xMOV(eax, r9d);
			xMOV(edx, eax);
			xSHR(edx, 23);
			xAND(edx, 0xff);
			xMOV(r11d, edx);

			xMOV(eax, r10d);
			xMOV(edx, eax);
			xSAR(edx, 31);
			xAND(eax, 0x7fffff);
			xOR(eax, 0x800000);
			xXOR(eax, edx);
			xSUB(eax, edx);
			xSHL(eax, 6);
			xSAR(eax, cl);
			xMOV(r10d, eax);

			xMOV(eax, r9d);
			xMOV(edx, eax);
			xSAR(edx, 31);
			xAND(eax, 0x7fffff);
			xOR(eax, 0x800000);
			xXOR(eax, edx);
			xSUB(eax, edx);
			xSHL(eax, 6);
			xADD(eax, r10d);
			xMOV(edx, eax);
			xAND(edx, 0x80000000);
			xMOV(r9d, edx);
			xMOV(edx, eax);
			xSAR(edx, 31);
			xXOR(eax, edx);
			xSUB(eax, edx);
			xForwardJZ32 add_result_zero;

			xBSR(ecx, eax);
			xADD(r11d, ecx);
			xSUB(r11d, 29);
			xCMP(ecx, 23);
			xForwardJLE32 add_normalize_left;
			xSUB(ecx, 23);
			xSHR(eax, cl);
			xForwardJump32 add_normalized;
			add_normalize_left.SetTarget();
			xNEG(ecx);
			xADD(ecx, 23);
			xSHL(eax, cl);
			add_normalized.SetTarget();
			xAND(eax, 0x7fffff);
			xMOV(edx, r11d);
			xCMP(edx, 255);
			xForwardJG32 add_overflow_result;
			xCMP(edx, 1);
			xForwardJL32 add_underflow_result;
			xSHL(edx, 23);
			xOR(eax, edx);
			xOR(eax, r9d);
			xXOR(r11d, r11d);
			xForwardJump32 add_result_ready;

			add_overflow_result.SetTarget();
			xMOV(eax, r9d);
			xOR(eax, 0x7fffffff);
			xMOV(r11d, result_overflow);
			xForwardJump32 add_result_ready_from_overflow;
			add_underflow_result.SetTarget();
			xOR(eax, r9d);
			xMOV(r11d, result_underflow);
			xForwardJump32 add_result_ready_from_underflow;
			add_result_zero.SetTarget();
			xXOR(eax, eax);
			xXOR(r11d, r11d);
			xForwardJump32 add_result_ready_from_zero;

			add_acc_denormal.SetTarget();
			xTEST(edx, edx);
			xForwardJZ32 add_both_denormal;
			xMOV(eax, r10d);
			xForwardJump32 add_result_ready_from_acc_denormal;
			add_both_denormal.SetTarget();
			xMOV(eax, r9d);
			xAND(eax, 0x80000000);
			xAND(eax, r10d);
			xForwardJump32 add_result_ready_from_both_denormal;
			add_product_denormal.SetTarget();
			add_result_acc_large_diff.SetTarget();
			xMOV(eax, r9d);
			xForwardJump32 add_result_ready_from_acc;
			add_result_product_large_diff.SetTarget();
			xMOV(eax, r10d);

			add_result_ready.SetTarget();
			add_result_ready_from_overflow.SetTarget();
			add_result_ready_from_underflow.SetTarget();
			add_result_ready_from_zero.SetTarget();
			add_result_ready_from_acc_denormal.SetTarget();
			add_result_ready_from_both_denormal.SetTarget();
			add_result_ready_from_acc.SetTarget();
			add_result_ready_from_acc_overflow.SetTarget();
			add_result_ready_from_mac_exception.SetTarget();
			xMOV(edx, r11d);
			xRET();
		}
	}

	{
		const auto generate_div_kernel = [](bool use_cap_exit) {
			// Internal ABI: eax = dividend, edx = divisor. Returns eax = raw
			// quotient and edx = exception flags: O=1, U=2, D=4, I=8.
			constexpr sptr div_fs_raw = 0;
			constexpr int div_ft_raw = div_fs_raw + 4;
			constexpr int div_result_exp = div_ft_raw + 4;
			constexpr int div_result_flags = div_result_exp + 4;
			constexpr int div_saved_rbp = div_result_flags + 8;
			constexpr int div_saved_rsi = div_saved_rbp + 8;
			constexpr int div_stack_size = div_saved_rsi + 8;
			constexpr int div_overflow = 1;
			constexpr int div_underflow = 2;
			constexpr int div_by_zero = 4;
			constexpr int div_invalid = 8;
			std::optional<xForwardJump32> div_cap_quotient_ready;

			const void* const entry = xGetAlignedCallTarget();
			xSUB(rsp, div_stack_size);
			xMOV(ptr32[rsp + div_fs_raw], eax);
			xMOV(ptr32[rsp + div_ft_raw], edx);
			xMOV(ptr32[rsp + div_result_flags], 0);
			xMOV(ptr64[rsp + div_saved_rbp], rbp);
			xMOV(ptr64[rsp + div_saved_rsi], rsi);

			xMOV(ecx, eax);
			xAND(ecx, 0x7f800000);
			xMOV(r8d, edx);
			xAND(r8d, 0x7f800000);
			xTEST(r8d, r8d);
			xForwardJNZ32 div_divisor_normal;
			xMOV(eax, ptr32[rsp + div_fs_raw]);
			xXOR(eax, ptr32[rsp + div_ft_raw]);
			xAND(eax, 0x80000000);
			xOR(eax, 0x7fffffff);
			xTEST(ecx, ecx);
			xForwardJNZ8 div_divide_by_zero_result;
			xMOV(ptr32[rsp + div_result_flags], div_invalid);
			xForwardJump32 div_result_ready_from_invalid;
			div_divide_by_zero_result.SetTarget();
			xMOV(ptr32[rsp + div_result_flags], div_by_zero);
			xForwardJump32 div_result_ready_from_divide_by_zero;

			div_divisor_normal.SetTarget();
			xTEST(ecx, ecx);
			xForwardJNZ32 div_dividend_normal;
			xMOV(eax, ptr32[rsp + div_fs_raw]);
			xXOR(eax, ptr32[rsp + div_ft_raw]);
			xAND(eax, 0x80000000);
			xForwardJump32 div_result_ready_from_zero;

			div_dividend_normal.SetTarget();
			xMOV(eax, ptr32[rsp + div_fs_raw]);
			xSHR(eax, 23);
			xAND(eax, 0xff);
			xMOV(edx, ptr32[rsp + div_ft_raw]);
			xSHR(edx, 23);
			xAND(edx, 0xff);
			xSUB(eax, edx);
			xADD(eax, 126);
			xMOV(ptr32[rsp + div_result_exp], eax);
			xCMP(eax, 255);
			xForwardJG32 div_overflow_result;
			xCMP(eax, 0);
			xForwardJL32 div_underflow_result;

			if (use_cap_exit)
			{
				xMOV(r9d, ptr32[rsp + div_fs_raw]);
				xAND(r9d, 0x7fffff);
				xOR(r9d, 0x800000);
				xMOV(r11d, ptr32[rsp + div_ft_raw]);
				xAND(r11d, 0x7fffff);
				xOR(r11d, 0x800000);
				X86SoftFloatEmitter::EmitSrtDivCapQuotient();
				xCMP(ecx, edx);
				xForwardJBE32 div_cap_fallback;
				xTEST(r8d, r8d);
				xForwardJNZ8 div_cap_exponent_ready;
				xINC(ptr32[rsp + div_result_exp]);
				div_cap_exponent_ready.SetTarget();
				div_cap_quotient_ready.emplace();
				div_cap_fallback.SetTarget();
			}
			xMOV(eax, ptr32[rsp + div_fs_raw]);
			xAND(eax, 0x7fffff);
			xOR(eax, 0x800000);
			xSHL(eax, 2);
			xMOV(r9d, eax);
			xMOV(eax, ptr32[rsp + div_ft_raw]);
			xAND(eax, 0x7fffff);
			xOR(eax, 0x800000);
			xSHL(eax, 2);
			xMOV(r11d, eax);
			xXOR(r10d, r10d);
			xXOR(ebp, ebp);
			xMOV(r8d, 1);

			xMOV(esi, 23);
			u8* const div_quotient_loop = xGetPtr();
			xSHL(ebp, 1);
			xADD(ebp, r8d);
			X86SoftFloatEmitter::EmitDivCarrySaveStep();
			xDEC(esi);
			xJcc32(Jcc_NotZero,
				static_cast<s32>(reinterpret_cast<sptr>(div_quotient_loop) -
								 (reinterpret_cast<sptr>(xGetPtr()) + 6)));

			xSHL(ebp, 1);
			xADD(ebp, r8d);
			X86SoftFloatEmitter::EmitDivCarrySaveStep();
			xMOV(eax, ebp);
			xSHL(eax, 1);
			xADD(eax, r8d);
			xCMP(eax, 1 << 24);
			xForwardJL8 div_quotient_normalized;
			xSHR(eax, 1);
			xINC(ptr32[rsp + div_result_exp]);
			div_quotient_normalized.SetTarget();
			if (div_cap_quotient_ready.has_value())
				div_cap_quotient_ready->SetTarget();
			xMOV(edx, ptr32[rsp + div_result_exp]);
			xCMP(edx, 255);
			xForwardJG32 div_overflow_after_normalize;
			xCMP(edx, 1);
			xForwardJL32 div_underflow_after_normalize;
			xAND(eax, 0x7fffff);
			xSHL(edx, 23);
			xOR(eax, edx);
			xMOV(ecx, ptr32[rsp + div_fs_raw]);
			xXOR(ecx, ptr32[rsp + div_ft_raw]);
			xAND(ecx, 0x80000000);
			xOR(eax, ecx);
			xForwardJump32 div_result_ready;

			div_overflow_result.SetTarget();
			div_overflow_after_normalize.SetTarget();
			xMOV(eax, ptr32[rsp + div_fs_raw]);
			xXOR(eax, ptr32[rsp + div_ft_raw]);
			xAND(eax, 0x80000000);
			xOR(eax, 0x7fffffff);
			xMOV(ptr32[rsp + div_result_flags], div_overflow);
			xForwardJump32 div_result_ready_from_overflow;

			div_underflow_result.SetTarget();
			div_underflow_after_normalize.SetTarget();
			xMOV(eax, ptr32[rsp + div_fs_raw]);
			xXOR(eax, ptr32[rsp + div_ft_raw]);
			xAND(eax, 0x80000000);
			xMOV(ptr32[rsp + div_result_flags], div_underflow);

			div_result_ready.SetTarget();
			div_result_ready_from_invalid.SetTarget();
			div_result_ready_from_divide_by_zero.SetTarget();
			div_result_ready_from_zero.SetTarget();
			div_result_ready_from_overflow.SetTarget();
			xMOV(edx, ptr32[rsp + div_result_flags]);
			xMOV(rbp, ptr64[rsp + div_saved_rbp]);
			xMOV(rsi, ptr64[rsp + div_saved_rsi]);
			xADD(rsp, div_stack_size);
			xRET();
			return entry;
		};

		s_fpuSoftDivExact = generate_div_kernel(false);
		s_fpuSoftDivCapExact = generate_div_kernel(true);

		auto generate_sqrt_kernel = [&]() -> const void* {
			// Internal ABI: eax = raw radicand. Returns eax = raw square root and
			// edx = exception flags using the same O/U/D/I bit layout as DIV. The
			// normal path returns the chop-mode host floor only when an exact
			// residual cap proves that it equals the existing SRT result.
			constexpr sptr sqrt_raw = 0;
			constexpr int sqrt_result_flags = sqrt_raw + 4;
			constexpr int sqrt_saved_rbp = sqrt_result_flags + 8;
			constexpr int sqrt_saved_rsi = sqrt_saved_rbp + 8;
			constexpr int sqrt_saved_rdi = sqrt_saved_rsi + 8;
			constexpr int sqrt_saved_xmm0 = sqrt_saved_rdi + 8;
			constexpr int sqrt_stack_size = sqrt_saved_xmm0 + 16;
			constexpr int sqrt_invalid = 8;

			const void* const entry = xGetAlignedCallTarget();
			xSUB(rsp, sqrt_stack_size);
			xMOV(ptr32[rsp + sqrt_raw], eax);
			xMOV(ptr32[rsp + sqrt_result_flags], 0);
			xMOV(ptr64[rsp + sqrt_saved_rbp], rbp);
			xMOV(ptr64[rsp + sqrt_saved_rsi], rsi);
			xMOV(ptr64[rsp + sqrt_saved_rdi], rdi);
			xTEST(eax, 0x80000000);
			xForwardJZ8 sqrt_input_nonnegative;
			xMOV(ptr32[rsp + sqrt_result_flags], sqrt_invalid);
			sqrt_input_nonnegative.SetTarget();
			xTEST(eax, 0x7f800000);
			xForwardJNZ32 sqrt_input_normal;
			xXOR(eax, eax);
			xForwardJump32 sqrt_result_ready_from_zero;

			sqrt_input_normal.SetTarget();
			xMOV(edx, ptr32[rsp + sqrt_raw]);
			xAND(edx, 0x7f800000);
			xCMP(edx, 0x7f800000);
			xForwardJZ32 sqrt_cap_extended_input;

			// Preserve xmm0 because it may hold a live EE mapping at the call site.
			xMOVUPS(ptr[rsp + sqrt_saved_xmm0], xmm0);
			xMOVDZX(xmm0, ptr32[rsp + sqrt_raw]);
			xMOV64(r10, reinterpret_cast<uptr>(&s_pos[0]));
			xPAND(xmm0, ptr128[r10]);
			const bool switch_mxcsr =
				EmuConfig.Cpu.FPUFPCR.GetRoundMode() != FPRoundMode::ChopZero;
			if (switch_mxcsr)
			{
				s_fpuSoftSqrtChopMode = EmuConfig.Cpu.FPUFPCR;
				s_fpuSoftSqrtChopMode.SetRoundMode(FPRoundMode::ChopZero);
				xLDMXCSR(ptr32[&s_fpuSoftSqrtChopMode.bitmask]);
			}
			xSQRT.SS(xmm0, xmm0);
			if (switch_mxcsr)
				xLDMXCSR(ptr32[&EmuConfig.Cpu.FPUFPCR.bitmask]);
			xMOVD(r9d, xmm0);
			xMOVUPS(xmm0, ptr[rsp + sqrt_saved_xmm0]);

			xMOV(eax, r9d);
			xAND(eax, 0x7fffff);
			xOR(eax, 0x800000);
			xMOV(r8d, eax);
			xUMUL(r8);
			// (R + 1)^2 - X > 2^23 iff X <= R^2 + 2R - 2^23.
			xLEA(rax, ptr[r8 * 2 + rax - (1 << 23)]);
			xMOV(r10d, ptr32[rsp + sqrt_raw]);
			xAND(r10d, 0x7fffff);
			xOR(r10d, 0x800000);
			xSHL(r10, 23);
			xTEST(ptr32[rsp + sqrt_raw], 0x800000);
			xForwardJNZ8 sqrt_cap_radicand_ready;
			xADD(r10, r10);
			sqrt_cap_radicand_ready.SetTarget();
			xCMP(r10, rax);
			xForwardJA32 sqrt_cap_unsafe;
			xMOV(eax, r9d);
			xForwardJump32 sqrt_cap_result_ready;

			sqrt_cap_extended_input.SetTarget();
			sqrt_cap_unsafe.SetTarget();
			xMOV(eax, ptr32[rsp + sqrt_raw]);
			xAND(eax, 0x7fffff);
			xOR(eax, 0x800000);
			xSHL(eax, 1);
			xTEST(ptr32[rsp + sqrt_raw], 0x800000);
			xForwardJNZ8 sqrt_mantissa_ready;
			xSHL(eax, 1);
			sqrt_mantissa_ready.SetTarget();
			xMOV(r9d, eax);
			xXOR(r10d, r10d);
			xXOR(ebp, ebp);
			xMOV(r8d, 1);

			xXOR(esi, esi);
			u8* const sqrt_quotient_loop = xGetPtr();
			xMOV(ecx, 24);
			xSUB(ecx, esi);
			xMOV(eax, r8d);
			xSHL(eax, cl);
			xADD(eax, ebp);
			xMOV(edi, eax);
			xMOV(ecx, 25);
			xSUB(ecx, esi);
			xMOV(eax, r8d);
			xSHL(eax, cl);
			xADD(ebp, eax);
			X86SoftFloatEmitter::EmitSqrtCarrySaveStep();
			xINC(esi);
			xCMP(esi, 23);
			xJcc32(Jcc_Less,
				static_cast<s32>(reinterpret_cast<sptr>(sqrt_quotient_loop) -
								 (reinterpret_cast<sptr>(xGetPtr()) + 6)));

			xMOV(eax, r8d);
			xSHL(eax, 1);
			xADD(eax, ebp);
			xMOV(edi, eax);
			xMOV(eax, r8d);
			xSHL(eax, 2);
			xADD(ebp, eax);
			X86SoftFloatEmitter::EmitSqrtCarrySaveStep();
			xMOV(eax, r8d);
			xSHL(eax, 1);
			xADD(eax, ebp);
			xSHR(eax, 2);
			xAND(eax, 0x7fffff);
			xMOV(edx, ptr32[rsp + sqrt_raw]);
			xSHR(edx, 23);
			xAND(edx, 0xff);
			xADD(edx, 127);
			xSHR(edx, 1);
			xSHL(edx, 23);
			xOR(eax, edx);

			sqrt_cap_result_ready.SetTarget();
			sqrt_result_ready_from_zero.SetTarget();
			xMOV(edx, ptr32[rsp + sqrt_result_flags]);
			xMOV(rbp, ptr64[rsp + sqrt_saved_rbp]);
			xMOV(rsi, ptr64[rsp + sqrt_saved_rsi]);
			xMOV(rdi, ptr64[rsp + sqrt_saved_rdi]);
			xADD(rsp, sqrt_stack_size);
			xRET();
			return entry;
		};

		s_fpuSoftSqrtExact = generate_sqrt_kernel();

		{
			// Internal ABI: eax = numerator Fs, edx = radicand Ft. Returns eax = raw
			// result and edx = the combined O/U/D/I exception flags.
			constexpr sptr rsqrt_fs_raw = 0;
			constexpr int rsqrt_ft_raw = rsqrt_fs_raw + 4;
			constexpr int rsqrt_extra_flags = rsqrt_ft_raw + 4;
			constexpr int rsqrt_result_flags = rsqrt_extra_flags + 4;
			constexpr int rsqrt_stack_size = rsqrt_result_flags + 8;
			constexpr int rsqrt_overflow = 1;
			constexpr int rsqrt_underflow = 2;
			constexpr int rsqrt_div_by_zero = 4;
			constexpr int rsqrt_invalid = 8;

			s_fpuSoftRsqrtExact = xGetAlignedCallTarget();
			xSUB(rsp, rsqrt_stack_size);
			xMOV(ptr32[rsp + rsqrt_fs_raw], eax);
			xMOV(ptr32[rsp + rsqrt_ft_raw], edx);
			xMOV(ptr32[rsp + rsqrt_extra_flags], 0);
			xTEST(edx, 0x80000000);
			xForwardJZ8 rsqrt_radicand_nonnegative;
			xMOV(ptr32[rsp + rsqrt_extra_flags], rsqrt_invalid);
			rsqrt_radicand_nonnegative.SetTarget();

			xMOV(ecx, edx);
			xAND(ecx, 0x7fffffff);
			xTEST(ecx, 0x7f800000);
			xForwardJNZ32 rsqrt_radicand_normal;
			xMOV(eax, ptr32[rsp + rsqrt_fs_raw]);
			xAND(eax, 0x80000000);
			xOR(eax, 0x7fffffff);
			xMOV(ecx, ptr32[rsp + rsqrt_extra_flags]);
			xTEST(ptr32[rsp + rsqrt_fs_raw], 0x7f800000);
			xForwardJNZ8 rsqrt_zero_divisor_nonzero_numerator;
			xOR(ecx, rsqrt_invalid);
			xForwardJump8 rsqrt_zero_divisor_flags_ready;
			rsqrt_zero_divisor_nonzero_numerator.SetTarget();
			xOR(ecx, rsqrt_div_by_zero);
			rsqrt_zero_divisor_flags_ready.SetTarget();
			xMOV(ptr32[rsp + rsqrt_result_flags], ecx);
			xForwardJump32 rsqrt_result_ready_from_zero_divisor;

			rsqrt_radicand_normal.SetTarget();
			xTEST(ptr32[rsp + rsqrt_fs_raw], 0x7f800000);
			xForwardJNZ32 rsqrt_numerator_normal;
			xMOV(eax, ptr32[rsp + rsqrt_fs_raw]);
			xAND(eax, 0x80000000);
			xMOV(ecx, ptr32[rsp + rsqrt_extra_flags]);
			xMOV(ptr32[rsp + rsqrt_result_flags], ecx);
			xForwardJump32 rsqrt_result_ready_from_zero_numerator;

			rsqrt_numerator_normal.SetTarget();
			xMOV(eax, ptr32[rsp + rsqrt_ft_raw]);
			xSHR(eax, 23);
			xAND(eax, 0xff);
			xADD(eax, 127);
			xSHR(eax, 1);
			xMOV(ecx, ptr32[rsp + rsqrt_fs_raw]);
			xSHR(ecx, 23);
			xAND(ecx, 0xff);
			xSUB(ecx, eax);
			xADD(ecx, 126);
			xCMP(ecx, 255);
			xForwardJG32 rsqrt_overflow_result;
			xCMP(ecx, 0);
			xForwardJL32 rsqrt_underflow_result;

			xMOV(eax, ptr32[rsp + rsqrt_ft_raw]);
			xAND(eax, 0x7fffffff);
			xCALL(s_fpuSoftSqrtExact);
			xMOV(edx, eax);
			xMOV(eax, ptr32[rsp + rsqrt_fs_raw]);
			xCALL(s_fpuSoftDivExact);
			xOR(edx, ptr32[rsp + rsqrt_extra_flags]);
			xMOV(ptr32[rsp + rsqrt_result_flags], edx);
			xForwardJump32 rsqrt_result_ready;

			rsqrt_overflow_result.SetTarget();
			xMOV(eax, ptr32[rsp + rsqrt_fs_raw]);
			xAND(eax, 0x80000000);
			xOR(eax, 0x7fffffff);
			xMOV(ecx, ptr32[rsp + rsqrt_extra_flags]);
			xOR(ecx, rsqrt_overflow);
			xMOV(ptr32[rsp + rsqrt_result_flags], ecx);
			xForwardJump32 rsqrt_result_ready_from_overflow;

			rsqrt_underflow_result.SetTarget();
			xMOV(eax, ptr32[rsp + rsqrt_fs_raw]);
			xAND(eax, 0x80000000);
			xMOV(ecx, ptr32[rsp + rsqrt_extra_flags]);
			xOR(ecx, rsqrt_underflow);
			xMOV(ptr32[rsp + rsqrt_result_flags], ecx);

			rsqrt_result_ready.SetTarget();
			rsqrt_result_ready_from_zero_divisor.SetTarget();
			rsqrt_result_ready_from_zero_numerator.SetTarget();
			rsqrt_result_ready_from_overflow.SetTarget();
			xMOV(edx, ptr32[rsp + rsqrt_result_flags]);
			xADD(rsp, rsqrt_stack_size);
			xRET();
		}
	}
}
//------------------------------------------------------------------

//------------------------------------------------------------------
// *FPU Opcodes!*
//------------------------------------------------------------------

// Those opcode are marked as special ! But I don't understand why we can't run them in the interpreter
#ifndef FPU_RECOMPILE

REC_FPUFUNC(CFC1);
REC_FPUFUNC(CTC1);
REC_FPUFUNC(MFC1);
REC_FPUFUNC(MTC1);

#else

//------------------------------------------------------------------
// CFC1 / CTC1
//------------------------------------------------------------------
void recCFC1(void)
{
	if (!_Rt_)
		return;
	EE::Profiler.EmitOp(eeOpcode::CFC1);

	const int regt = _allocX86reg(X86TYPE_GPR, _Rt_, MODE_WRITE);
	if (_Fs_ >= 16)
	{
		xMOV(xRegister32(regt), ptr32[&fpuRegs.fprc[31]]);
		xAND(xRegister32(regt), 0x0083c078); //remove always-zero bits
		xOR(xRegister32(regt), 0x01000001); //set always-one bits
		xMOVSX(xRegister64(regt), xRegister32(regt));
	}
	else
	{
		xMOVSX(xRegister64(regt), ptr32[&fpuRegs.fprc[0]]);
	}
}

void recCTC1()
{
	if (_Fs_ != 31)
		return;
	EE::Profiler.EmitOp(eeOpcode::CTC1);

	if (GPR_IS_CONST1(_Rt_))
	{
		xMOV(ptr32[&fpuRegs.fprc[_Fs_]], g_cpuConstRegs[_Rt_].UL[0]);
	}
	else
	{
		int mmreg = _checkXMMreg(XMMTYPE_GPRREG, _Rt_, MODE_READ);

		if (mmreg >= 0)
		{
			xMOVSS(ptr[&fpuRegs.fprc[_Fs_]], xRegisterSSE(mmreg));
		}
		else if ((mmreg = _checkX86reg(X86TYPE_GPR, _Rt_, MODE_READ)) >= 0)
		{
			xMOV(ptr32[&fpuRegs.fprc[_Fs_]], xRegister32(mmreg));
		}
		else
		{
			_deleteGPRtoXMMreg(_Rt_, 1);

			xMOV(eax, ptr[&cpuRegs.GPR.r[_Rt_].UL[0]]);
			xMOV(ptr[&fpuRegs.fprc[_Fs_]], eax);
		}
	}
}
//------------------------------------------------------------------


//------------------------------------------------------------------
// MFC1
//------------------------------------------------------------------

void recMFC1()
{
	if (!_Rt_)
		return;

	EE::Profiler.EmitOp(eeOpcode::MFC1);

	const int xmmregt = _allocIfUsedGPRtoXMM(_Rt_, MODE_READ | MODE_WRITE);
	const int regs = _allocIfUsedFPUtoXMM(_Fs_, MODE_READ);
	if (regs >= 0 && xmmregt >= 0)
	{
		// if we're in xmm, we shouldn't be const
		pxAssert(!GPR_IS_CONST1(_Rt_));

		// both in xmm, sign extend and insert lower bits
		const int temp = _allocTempXMMreg(XMMT_FPS);
		xPSRA.D(xRegisterSSE(temp), xRegisterSSE(regs), 31);
		xMOVSS(xRegisterSSE(xmmregt), xRegisterSSE(regs));
		xINSERTPS(xRegisterSSE(xmmregt), xRegisterSSE(temp), _MM_MK_INSERTPS_NDX(0, 1, 0));
		_freeXMMreg(temp);
		return;
	}

	// storing to a gpr..
	const int regt = _allocX86reg(X86TYPE_GPR, _Rt_, MODE_WRITE);

	// shouldn't be const after we're writing.
	pxAssert(!GPR_IS_CONST1(_Rt_));

	if (regs >= 0)
	{
		// xmm -> gpr
		xMOVD(xRegister32(regt), xRegisterSSE(regs));
		xMOVSX(xRegister64(regt), xRegister32(regt));
	}
	else
	{
		// mem -> gpr
		xMOVSX(xRegister64(regt), ptr32[&fpuRegs.fpr[_Fs_].UL]);
	}
}

//------------------------------------------------------------------


//------------------------------------------------------------------
// MTC1
//------------------------------------------------------------------
void recMTC1()
{
	EE::Profiler.EmitOp(eeOpcode::MTC1);
	if (GPR_IS_CONST1(_Rt_))
	{
		const int xmmreg = _allocIfUsedFPUtoXMM(_Fs_, MODE_WRITE);
		if (xmmreg >= 0)
		{
			// common case: mtc1 zero, fnn
			if (g_cpuConstRegs[_Rt_].UL[0] == 0)
			{
				xPXOR(xRegisterSSE(xmmreg), xRegisterSSE(xmmreg));
			}
			else
			{
				// may as well flush the constant register, since we're needing it in a gpr anyway
				const int x86reg = _allocX86reg(X86TYPE_GPR, _Rt_, MODE_READ);
				xMOVDZX(xRegisterSSE(xmmreg), xRegister32(x86reg));
			}
		}
		else
		{
			pxAssert(!_hasXMMreg(XMMTYPE_FPREG, _Fs_));
			xMOV(ptr32[&fpuRegs.fpr[_Fs_].UL], g_cpuConstRegs[_Rt_].UL[0]);
		}
	}
	else
	{
		const int xmmgpr = _checkXMMreg(XMMTYPE_GPRREG, _Rt_, MODE_READ);
		if (xmmgpr >= 0)
		{
			if (g_pCurInstInfo->regs[_Rt_] & EEINST_LASTUSE)
			{
				// transfer the reg directly
				_deleteFPtoXMMreg(_Fs_, DELETE_REG_FREE_NO_WRITEBACK);
				_reallocateXMMreg(xmmgpr, XMMTYPE_FPREG, _Fs_, MODE_WRITE);
			}
			else
			{
				const int xmmreg2 = _allocIfUsedFPUtoXMM(_Fs_, MODE_WRITE);
				if (xmmreg2 >= 0)
					xMOVSS(xRegisterSSE(xmmreg2), xRegisterSSE(xmmgpr));
				else
					xMOVSS(ptr[&fpuRegs.fpr[_Fs_].UL], xRegisterSSE(xmmgpr));
			}
		}
		else
		{
			// may as well cache it..
			const int regt = _allocX86reg(X86TYPE_GPR, _Rt_, MODE_READ);
			const int mmreg2 = _allocIfUsedFPUtoXMM(_Fs_, MODE_WRITE);

			if (mmreg2 >= 0)
			{
				xMOVDZX(xRegisterSSE(mmreg2), xRegister32(regt));
			}
			else
			{
				xMOV(ptr32[&fpuRegs.fpr[_Fs_].UL], xRegister32(regt));
			}
		}
	}
}
#endif
//------------------------------------------------------------------


#ifndef FPU_RECOMPILE // If FPU_RECOMPILE is not defined, then use the interpreter opcodes. (CFC1, CTC1, MFC1, and MTC1 are special because they work specifically with the EE rec so they're defined above)

REC_FPUFUNC(ABS_S);
REC_FPUFUNC(ADD_S);
REC_FPUFUNC(ADDA_S);
REC_FPUBRANCH(BC1F);
REC_FPUBRANCH(BC1T);
REC_FPUBRANCH(BC1FL);
REC_FPUBRANCH(BC1TL);
REC_FPUFUNC(C_EQ);
REC_FPUFUNC(C_F);
REC_FPUFUNC(C_LE);
REC_FPUFUNC(C_LT);
REC_FPUFUNC(CVT_S);
REC_FPUFUNC(CVT_W);
REC_FPUFUNC(DIV_S);
REC_FPUFUNC(MAX_S);
REC_FPUFUNC(MIN_S);
REC_FPUFUNC(MADD_S);
REC_FPUFUNC(MADDA_S);
REC_FPUFUNC(MOV_S);
REC_FPUFUNC(MSUB_S);
REC_FPUFUNC(MSUBA_S);
REC_FPUFUNC(MUL_S);
REC_FPUFUNC(MULA_S);
REC_FPUFUNC(NEG_S);
REC_FPUFUNC(SUB_S);
REC_FPUFUNC(SUBA_S);
REC_FPUFUNC(SQRT_S);
REC_FPUFUNC(RSQRT_S);

#else // FPU_RECOMPILE

//------------------------------------------------------------------
// Clamp Functions (Converts NaN's and Infinities to Normal Numbers)
//------------------------------------------------------------------

static int fpuCopyToTempForClamp(int fpureg, int xmmreg)
{
	if (FPUINST_USEDTEST(fpureg))
	{
		const int tempreg = _allocTempXMMreg(XMMT_FPS);
		xMOVSS(xRegisterSSE(tempreg), xRegisterSSE(xmmreg));
		return tempreg;
	}

	// flush back the original value, before we mess with it below
	if (FPUINST_LIVETEST(fpureg))
		_flushXMMreg(xmmreg);

	// turn it into a temp, so in case the liveness was incorrect, we don't reuse it after clamp
	_reallocateXMMreg(xmmreg, XMMTYPE_TEMP, 0, 0, true);
	return xmmreg;
}

static void fpuFreeIfTemp(int xmmreg)
{
	if (xmmregs[xmmreg].inuse && xmmregs[xmmreg].type == XMMTYPE_TEMP)
		_freeXMMreg(xmmreg);
}

__fi void fpuFloat3(int regd) // +NaN -> +fMax, -NaN -> -fMax, +Inf -> +fMax, -Inf -> -fMax
{
	xPMIN.SD(xRegisterSSE(regd), ptr128[&g_maxvals[0]]);
	xPMIN.UD(xRegisterSSE(regd), ptr128[&g_minvals[0]]);
}

__fi void fpuFloat(int regd) // +/-NaN -> +fMax, +Inf -> +fMax, -Inf -> -fMax
{
	if (CHECK_FPU_OVERFLOW)
	{
		xMIN.SS(xRegisterSSE(regd), ptr[&g_maxvals[0]]); // MIN() must be before MAX()! So that NaN's become +Maximum
		xMAX.SS(xRegisterSSE(regd), ptr[&g_minvals[0]]);
	}
}

__fi void fpuFloat2(int regd) // +NaN -> +fMax, -NaN -> -fMax, +Inf -> +fMax, -Inf -> -fMax
{
	if (CHECK_FPU_OVERFLOW)
	{
		fpuFloat3(regd);
	}
}

void ClampValues(int regd)
{
	fpuFloat(regd);
}
//------------------------------------------------------------------


//------------------------------------------------------------------
// ABS XMM
//------------------------------------------------------------------
void recABS_S_xmm(int info)
{
	EE::Profiler.EmitOp(eeOpcode::ABS_F);
	if (info & PROCESS_EE_S)
		xMOVSS(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_S));
	else
		xMOVSSZX(xRegisterSSE(EEREC_D), ptr[&fpuRegs.fpr[_Fs_]]);

	xAND.PS(xRegisterSSE(EEREC_D), ptr[&s_pos[0]]);
	//xAND(ptr32[&fpuRegs.fprc[31]], ~(FPUflagO|FPUflagU)); // Clear O and U flags

	if (CHECK_FPU_OVERFLOW) // Only need to do positive clamp, since EEREC_D is positive
		xMIN.SS(xRegisterSSE(EEREC_D), ptr[&g_maxvals[0]]);
}

FPURECOMPILE_CONSTCODE(ABS_S, XMMINFO_WRITED | XMMINFO_READS);
//------------------------------------------------------------------


//------------------------------------------------------------------
// FPU_ADD_SUB (Used to mimic PS2's FPU add/sub behavior)
//------------------------------------------------------------------
// Compliant IEEE FPU uses, in computations, uses additional "guard" bits to the right of the mantissa
// but EE-FPU doesn't. Substraction (and addition of positive and negative) may shift the mantissa left,
// causing those bits to appear in the result; this function masks out the bits of the mantissa that will
// get shifted right to the guard bits to ensure that the guard bits are empty.
// The difference of the exponents = the amount that the smaller operand will be shifted right by.
// Modification - the PS2 uses a single guard bit? (Coded by Nneeve)
//------------------------------------------------------------------
void FPU_ADD_SUB(int regd, int regt, int issub)
{
	const int xmmtemp = _allocTempXMMreg(XMMT_FPS); //temporary for anding with regd/regt
	xMOVD(ecx, xRegisterSSE(regd)); // ecx receives regd
	xMOVD(eax, xRegisterSSE(regt)); // eax receives regt

	//mask the exponents
	xSHR(ecx, 23);
	xSHR(eax, 23);
	xAND(ecx, 0xff);
	xAND(eax, 0xff);

	xSUB(ecx, eax); //tempecx = exponent difference
	xCMP(ecx, 25);
	j8Ptr[0] = JGE8(0);
	xCMP(ecx, 0);
	j8Ptr[1] = JG8(0);
	j8Ptr[2] = JE8(0);
	xCMP(ecx, -25);
	j8Ptr[3] = JLE8(0);

	//diff = -24 .. -1 , expd < expt
	xNEG(ecx);
	xDEC(ecx);
	xMOV(eax, 0xffffffff);
	xSHL(eax, cl); //temp2 = 0xffffffff << tempecx
	xMOVDZX(xRegisterSSE(xmmtemp), eax);
	xAND.PS(xRegisterSSE(regd), xRegisterSSE(xmmtemp));
	if (issub)
		xSUB.SS(xRegisterSSE(regd), xRegisterSSE(regt));
	else
		xADD.SS(xRegisterSSE(regd), xRegisterSSE(regt));
	j8Ptr[4] = JMP8(0);

	x86SetJ8(j8Ptr[0]);
	//diff = 25 .. 255 , expt < expd
	xAND.PS(xRegisterSSE(xmmtemp), xRegisterSSE(regt), ptr[s_neg]);
	if (issub)
		xSUB.SS(xRegisterSSE(regd), xRegisterSSE(xmmtemp));
	else
		xADD.SS(xRegisterSSE(regd), xRegisterSSE(xmmtemp));
	j8Ptr[5] = JMP8(0);

	x86SetJ8(j8Ptr[1]);
	//diff = 1 .. 24, expt < expd
	xDEC(ecx);
	xMOV(eax, 0xffffffff);
	xSHL(eax, cl); //temp2 = 0xffffffff << tempecx
	xMOVDZX(xRegisterSSE(xmmtemp), eax);
	xAND.PS(xRegisterSSE(xmmtemp), xRegisterSSE(regt));
	if (issub)
		xSUB.SS(xRegisterSSE(regd), xRegisterSSE(xmmtemp));
	else
		xADD.SS(xRegisterSSE(regd), xRegisterSSE(xmmtemp));
	j8Ptr[6] = JMP8(0);

	x86SetJ8(j8Ptr[3]);
	//diff = -255 .. -25, expd < expt
	xAND.PS(xRegisterSSE(regd), ptr[s_neg]);
	if (issub)
		xSUB.SS(xRegisterSSE(regd), xRegisterSSE(regt));
	else
		xADD.SS(xRegisterSSE(regd), xRegisterSSE(regt));
	j8Ptr[7] = JMP8(0);

	x86SetJ8(j8Ptr[2]);
	//diff == 0
	if (issub)
		xSUB.SS(xRegisterSSE(regd), xRegisterSSE(regt));
	else
		xADD.SS(xRegisterSSE(regd), xRegisterSSE(regt));

	x86SetJ8(j8Ptr[4]);
	x86SetJ8(j8Ptr[5]);
	x86SetJ8(j8Ptr[6]);
	x86SetJ8(j8Ptr[7]);

	_freeXMMreg(xmmtemp);
}

void FPU_ADD(int regd, int regt)
{
	if (FPU_CORRECT_ADD_SUB)
		FPU_ADD_SUB(regd, regt, 0);
	else
		xADD.SS(xRegisterSSE(regd), xRegisterSSE(regt));
}

void FPU_SUB(int regd, int regt)
{
	if (FPU_CORRECT_ADD_SUB)
		FPU_ADD_SUB(regd, regt, 1);
	else
		xSUB.SS(xRegisterSSE(regd), xRegisterSSE(regt));
}

//------------------------------------------------------------------
// Note: PS2's multiplication uses some variant of booth multiplication with wallace trees:
// It cuts off some bits, resulting in inaccurate and non-commutative results.
// The PS2's result mantissa is either equal to x86's rounding to zero result mantissa
// or SMALLER (by 0x1). (this means that x86's other rounding modes are only less similar to PS2's mul)
//------------------------------------------------------------------

void FPU_MUL(int regd, int regt, bool reverseOperands)
{
	u8 *endMul = nullptr;

	if (CHECK_FPUMULHACK)
	{
		// 	if ((s == 0x3e800000) && (t == 0x40490fdb))
		// 		return 0x3f490fda; // needed for Tales of Destiny Remake (only in a very specific room late-game)
		// 	else
		// 		return 0;

		alignas(16) static constexpr const u32 result[4] = { 0x3f490fda };

		xMOVD(ecx, xRegisterSSE(reverseOperands ? regt : regd));
		xMOVD(edx, xRegisterSSE(reverseOperands ? regd : regt));

		// if (((s ^ 0x3e800000) | (t ^ 0x40490fdb)) != 0) { hack; }
		xXOR(ecx, 0x3e800000);
		xXOR(edx, 0x40490fdb);
		xOR(edx, ecx);

		u8* noHack = JNZ8(0);
			xMOVAPS(xRegisterSSE(regd), ptr128[result]);
			endMul = JMP8(0);
		x86SetJ8(noHack);
	}

	xMUL.SS(xRegisterSSE(regd), xRegisterSSE(regt));

	if (CHECK_FPUMULHACK)
		x86SetJ8(endMul);
}

void FPU_MUL(int regd, int regt) { FPU_MUL(regd, regt, false); }
void FPU_MUL_REV(int regd, int regt) { FPU_MUL(regd, regt, true); } //reversed operands

//------------------------------------------------------------------
// CommutativeOp XMM (used for ADD, MUL, MAX, and MIN opcodes)
//------------------------------------------------------------------
static void (*recComOpXMM_to_XMM[])(x86SSERegType, x86SSERegType) = {
	FPU_ADD, FPU_MUL,     SSE_MAXSS_XMM_to_XMM, SSE_MINSS_XMM_to_XMM};

static void (*recComOpXMM_to_XMM_REV[])(x86SSERegType, x86SSERegType) = { //reversed operands
	FPU_ADD, FPU_MUL_REV, SSE_MAXSS_XMM_to_XMM, SSE_MINSS_XMM_to_XMM};

//static void (*recComOpM32_to_XMM[] )(x86SSERegType, uptr) = {
//	SSE_ADDSS_M32_to_XMM, SSE_MULSS_M32_to_XMM, SSE_MAXSS_M32_to_XMM, SSE_MINSS_M32_to_XMM };

int recCommutativeOp(int info, int regd, int op)
{
	int t0reg = _allocTempXMMreg(XMMT_FPS);

	switch (info & (PROCESS_EE_S | PROCESS_EE_T))
	{
		case PROCESS_EE_S:
			if (regd == EEREC_S)
			{
				xMOVSSZX(xRegisterSSE(t0reg), ptr[&fpuRegs.fpr[_Ft_]]);
				if (CHECK_FPU_EXTRA_OVERFLOW /*&& !CHECK_FPUCLAMPHACK */ || (op >= 2))
				{
					fpuFloat2(regd);
					fpuFloat2(t0reg);
				}
				recComOpXMM_to_XMM[op](regd, t0reg);
			}
			else
			{
				xMOVSSZX(xRegisterSSE(regd), ptr[&fpuRegs.fpr[_Ft_]]);
				if (CHECK_FPU_EXTRA_OVERFLOW || (op >= 2))
				{
					fpuFloat2(regd);
					fpuFloat2(EEREC_S);
				}
				recComOpXMM_to_XMM_REV[op](regd, EEREC_S);
			}
			break;
		case PROCESS_EE_T:
			if (regd == EEREC_T)
			{
				xMOVSSZX(xRegisterSSE(t0reg), ptr[&fpuRegs.fpr[_Fs_]]);
				if (CHECK_FPU_EXTRA_OVERFLOW || (op >= 2))
				{
					fpuFloat2(regd);
					fpuFloat2(t0reg);
				}
				recComOpXMM_to_XMM_REV[op](regd, t0reg);
			}
			else
			{
				xMOVSSZX(xRegisterSSE(regd), ptr[&fpuRegs.fpr[_Fs_]]);
				if (CHECK_FPU_EXTRA_OVERFLOW || (op >= 2))
				{
					fpuFloat2(regd);
					fpuFloat2(EEREC_T);
				}
				recComOpXMM_to_XMM[op](regd, EEREC_T);
			}
			break;
		case (PROCESS_EE_S | PROCESS_EE_T):
			if (regd == EEREC_T)
			{
				if (CHECK_FPU_EXTRA_OVERFLOW || (op >= 2))
				{
					fpuFloat2(regd);
					fpuFloat2(EEREC_S);
				}
				recComOpXMM_to_XMM_REV[op](regd, EEREC_S);
			}
			else
			{
				xMOVSS(xRegisterSSE(regd), xRegisterSSE(EEREC_S));
				if (CHECK_FPU_EXTRA_OVERFLOW || (op >= 2))
				{
					fpuFloat2(regd);
					fpuFloat2(EEREC_T);
				}
				recComOpXMM_to_XMM[op](regd, EEREC_T);
			}
			break;
		default:
			Console.WriteLn(Color_Magenta, "FPU: recCommutativeOp case 4");
			xMOVSSZX(xRegisterSSE(regd), ptr[&fpuRegs.fpr[_Fs_]]);
			xMOVSSZX(xRegisterSSE(t0reg), ptr[&fpuRegs.fpr[_Ft_]]);
			if (CHECK_FPU_EXTRA_OVERFLOW || (op >= 2))
			{
				fpuFloat2(regd);
				fpuFloat2(t0reg);
			}
			recComOpXMM_to_XMM[op](regd, t0reg);
			break;
	}

	_freeXMMreg(t0reg);
	return regd;
}
//------------------------------------------------------------------


//------------------------------------------------------------------
// ADD XMM
//------------------------------------------------------------------
static void fpuPrepareSoftKernelCall()
{
	for (u32 i = 0; i < iREGCNT_GPR; i++)
	{
		if (!x86regs[i].inuse || !xRegisterBase::IsCallerSaved(i))
			continue;

		_freeX86reg(i);
	}
}



static void fpuCommitSoftOverflowUnderflowFlags()
{
	xTEST(edx, 1);
	xForwardJZ8 result_not_overflow;
	xOR(ptr32[&fpuRegs.fprc[31]], FPUflagO | FPUflagSO);
	xForwardJump8 result_flags_ready_from_overflow;

	result_not_overflow.SetTarget();
	xAND(ptr32[&fpuRegs.fprc[31]], ~FPUflagO);
	xTEST(edx, 2);
	xForwardJZ8 result_not_underflow;
	xOR(ptr32[&fpuRegs.fprc[31]], FPUflagU | FPUflagSU);
	xForwardJump8 result_flags_ready_from_underflow;

	result_not_underflow.SetTarget();
	xAND(ptr32[&fpuRegs.fprc[31]], ~FPUflagU);
	result_flags_ready_from_overflow.SetTarget();
	result_flags_ready_from_underflow.SetTarget();
}

template <bool writes_acc>
static void fpuCommitSoftMaddFlags()
{
	xTEST(edx, 1);
	xForwardJZ8 madd_not_overflow;
	xOR(ptr32[&fpuRegs.fprc[31]], FPUflagO | FPUflagSO);
	xForwardJump8 madd_flags_ready_from_overflow;

	madd_not_overflow.SetTarget();
	xAND(ptr32[&fpuRegs.fprc[31]], ~FPUflagO);
	if constexpr (writes_acc)
	{
		xTEST(edx, 2);
		xForwardJZ8 madd_acc_result_not_underflow;
		xOR(ptr32[&fpuRegs.fprc[31]], FPUflagSU);
		madd_acc_result_not_underflow.SetTarget();
	}
	else
	{
		xTEST(edx, 2);
		xForwardJZ8 madd_result_not_underflow;
		xOR(ptr32[&fpuRegs.fprc[31]], FPUflagU | FPUflagSU);
		xForwardJump8 madd_result_underflow_ready;
		madd_result_not_underflow.SetTarget();
		xAND(ptr32[&fpuRegs.fprc[31]], ~FPUflagU);
		madd_result_underflow_ready.SetTarget();
	}
	xTEST(r8d, 2);
	xForwardJZ8 madd_product_not_underflow;
	xOR(ptr32[&fpuRegs.fprc[31]], FPUflagSU);
	madd_product_not_underflow.SetTarget();
	madd_flags_ready_from_overflow.SetTarget();
}

static void fpuCommitSoftDivideInvalidFlags(bool clear_causes)
{
	if (clear_causes)
		xAND(ptr32[&fpuRegs.fprc[31]], ~(FPUflagD | FPUflagI));
	xTEST(edx, 4);
	xForwardJZ8 result_not_divide_by_zero;
	xOR(ptr32[&fpuRegs.fprc[31]], FPUflagD | FPUflagSD);
	result_not_divide_by_zero.SetTarget();
	xTEST(edx, 8);
	xForwardJZ8 result_not_invalid;
	xOR(ptr32[&fpuRegs.fprc[31]], FPUflagI | FPUflagSI);
	result_not_invalid.SetTarget();
}

static void fpuUpdateNativeAccOverflow(int acc_reg)
{
	const int temp = _allocX86reg(X86TYPE_TEMP, 0, 0);
	xMOVD(xRegister32(temp), xRegisterSSE(acc_reg));
	xAND(xRegister32(temp), 0x7fffffff);
	xMOV(ptr32[&fpuRegs.ACCflag], 0);
	xCMP(xRegister32(temp), 0x7f800000);
	xForwardJNE8 acc_not_overflow;
	xMOV(ptr32[&fpuRegs.ACCflag], 1);
	acc_not_overflow.SetTarget();
	_freeX86reg(temp);
}

static void fpuLoadSoftOperand(
	const xRegister32& dst, int info, int process_flag, int fpureg, int xmmreg)
{
	if (info & process_flag)
		xMOVD(dst, xRegisterSSE(xmmreg));
	else
		xMOV(dst, ptr32[&fpuRegs.fpr[fpureg]]);
}

template <eeOpcode opcode, bool subtract, bool writes_acc>
static void recSoftAddSub(int info)
{
	EE::Profiler.EmitOp(opcode);
	fpuPrepareSoftKernelCall();
	fpuLoadSoftOperand(eax, info, PROCESS_EE_S, _Fs_, EEREC_S);
	fpuLoadSoftOperand(edx, info, PROCESS_EE_T, _Ft_, EEREC_T);
	xCALL(s_fpuSoftAddSubExact[subtract ? 1 : 0]);
	const int destination = writes_acc ? EEREC_ACC : EEREC_D;
	xMOVDZX(xRegisterSSE(destination), eax);
	if constexpr (writes_acc)
	{
		xMOV(ecx, edx);
		xAND(ecx, 1);
		xMOV(ptr32[&fpuRegs.ACCflag], ecx);
	}
	fpuCommitSoftOverflowUnderflowFlags();
}


template <eeOpcode opcode, bool writes_acc>
static void recSoftMul(int info)
{
	EE::Profiler.EmitOp(opcode);
	fpuPrepareSoftKernelCall();
	fpuLoadSoftOperand(eax, info, PROCESS_EE_S, _Fs_, EEREC_S);
	fpuLoadSoftOperand(edx, info, PROCESS_EE_T, _Ft_, EEREC_T);
	xCALL(s_fpuSoftMulExact);
	const int destination = writes_acc ? EEREC_ACC : EEREC_D;
	xMOVDZX(xRegisterSSE(destination), eax);
	if constexpr (writes_acc)
	{
		xMOV(ecx, edx);
		xAND(ecx, 1);
		xMOV(ptr32[&fpuRegs.ACCflag], ecx);
	}
	fpuCommitSoftOverflowUnderflowFlags();
}

template <eeOpcode opcode, bool subtract, bool writes_acc>
static void recSoftMadd(int info)
{
	EE::Profiler.EmitOp(opcode);
	fpuPrepareSoftKernelCall();
	fpuLoadSoftOperand(eax, info, PROCESS_EE_S, _Fs_, EEREC_S);
	fpuLoadSoftOperand(edx, info, PROCESS_EE_T, _Ft_, EEREC_T);
	xCALL(s_fpuSoftMulExact);
	xMOV(r8d, edx);
	xMOV(ecx, edx);
	xMOV(edx, eax);
	if (info & PROCESS_EE_ACC)
		xMOVD(eax, xRegisterSSE(EEREC_ACC));
	else
		xMOV(eax, ptr32[&fpuRegs.ACC]);
	xMOV(r9d, ptr32[&fpuRegs.ACCflag]);
	xCALL(s_fpuSoftMaddExact[subtract ? 1 : 0]);
	const int destination = writes_acc ? EEREC_ACC : EEREC_D;
	xMOVDZX(xRegisterSSE(destination), eax);
	if constexpr (writes_acc)
	{
		xMOV(ecx, edx);
		xAND(ecx, 1);
		xMOV(ptr32[&fpuRegs.ACCflag], ecx);
	}
	fpuCommitSoftMaddFlags<writes_acc>();
}

static void recSoftDiv(int info)
{
	EE::Profiler.EmitOp(eeOpcode::DIV_F);
	fpuPrepareSoftKernelCall();
	fpuLoadSoftOperand(eax, info, PROCESS_EE_S, _Fs_, EEREC_S);
	fpuLoadSoftOperand(edx, info, PROCESS_EE_T, _Ft_, EEREC_T);
	xCALL(s_fpuSoftDivCapExact);
	xMOVDZX(xRegisterSSE(EEREC_D), eax);
	fpuCommitSoftDivideInvalidFlags(false);
}

static void recSoftSqrt(int info)
{
	EE::Profiler.EmitOp(eeOpcode::SQRT_F);
	fpuPrepareSoftKernelCall();
	fpuLoadSoftOperand(eax, info, PROCESS_EE_T, _Ft_, EEREC_T);
	xCALL(s_fpuSoftSqrtExact);
	xMOVDZX(xRegisterSSE(EEREC_D), eax);
	xAND(ptr32[&fpuRegs.fprc[31]], ~(FPUflagD | FPUflagI));
	xTEST(edx, 8);
	xForwardJZ8 sqrt_result_not_invalid;
	xOR(ptr32[&fpuRegs.fprc[31]], FPUflagI | FPUflagSI);
	xForwardJump8 sqrt_flags_ready_from_invalid;
	sqrt_result_not_invalid.SetTarget();
	fpuCommitSoftOverflowUnderflowFlags();
	sqrt_flags_ready_from_invalid.SetTarget();
}

static void recSoftRsqrt(int info)
{
	EE::Profiler.EmitOp(eeOpcode::RSQRT_F);
	fpuPrepareSoftKernelCall();
	fpuLoadSoftOperand(eax, info, PROCESS_EE_S, _Fs_, EEREC_S);
	fpuLoadSoftOperand(edx, info, PROCESS_EE_T, _Ft_, EEREC_T);
	xCALL(s_fpuSoftRsqrtExact);
	xMOVDZX(xRegisterSSE(EEREC_D), eax);
	fpuCommitSoftDivideInvalidFlags(true);
}

void recADD_S_xmm(int info)
{
	EE::Profiler.EmitOp(eeOpcode::ADD_F);
	//xAND(ptr32[&fpuRegs.fprc[31]], ~(FPUflagO|FPUflagU)); // Clear O and U flags
	ClampValues(recCommutativeOp(info, EEREC_D, 0));
	//REC_FPUOP(ADD_S);
}

FPURECOMPILE_CONSTCODE_EXACT(
	ADD_S, XMMINFO_WRITED | XMMINFO_READS | XMMINFO_READT, recSoftAddSub<eeOpcode::ADD_F, false, false>);

void recADDA_S_xmm(int info)
{
	EE::Profiler.EmitOp(eeOpcode::ADDA_F);
	//xAND(ptr32[&fpuRegs.fprc[31]], ~(FPUflagO|FPUflagU)); // Clear O and U flags
	const int regd = recCommutativeOp(info, EEREC_ACC, 0);
	fpuUpdateNativeAccOverflow(regd);
	ClampValues(regd);
}

FPURECOMPILE_CONSTCODE_EXACT(
	ADDA_S, XMMINFO_WRITEACC | XMMINFO_READS | XMMINFO_READT, recSoftAddSub<eeOpcode::ADDA_F, false, true>);
//------------------------------------------------------------------

//------------------------------------------------------------------
// BC1x XMM
//------------------------------------------------------------------

static void _setupBranchTest()
{
	_eeFlushAllDirty();

	// COP1 branch conditionals are based on the following equation:
	// (fpuRegs.fprc[31] & 0x00800000)
	// BC2F checks if the statement is false, BC2T checks if the statement is true.

	xMOV(eax, ptr[&fpuRegs.fprc[31]]);
	xTEST(eax, FPUflagC);
}

void recBC1F()
{
	EE::Profiler.EmitOp(eeOpcode::BC1F);
	const u32 branchTo = ((s32)_Imm_ * 4) + pc;
	const bool swap = TrySwapDelaySlot(0, 0, 0, true);
	_setupBranchTest();
	recDoBranchImm(branchTo, JNZ32(0), false, swap);
}

void recBC1T()
{
	EE::Profiler.EmitOp(eeOpcode::BC1T);
	const u32 branchTo = ((s32)_Imm_ * 4) + pc;
	const bool swap = TrySwapDelaySlot(0, 0, 0, true);
	_setupBranchTest();
	recDoBranchImm(branchTo, JZ32(0), false, swap);
}

void recBC1FL()
{
	EE::Profiler.EmitOp(eeOpcode::BC1FL);
	const u32 branchTo = ((s32)_Imm_ * 4) + pc;
	_setupBranchTest();
	recDoBranchImm(branchTo, JNZ32(0), true, false);
}

void recBC1TL()
{
	EE::Profiler.EmitOp(eeOpcode::BC1TL);
	const u32 branchTo = ((s32)_Imm_ * 4) + pc;
	_setupBranchTest();
	recDoBranchImm(branchTo, JZ32(0), true, false);
}
//------------------------------------------------------------------


//------------------------------------------------------------------
// C.x.S XMM
//------------------------------------------------------------------
static void fpuLoadSoftCompareValue(
	const xRegister32& dst, int info, int process_flag, int fpureg, int xmmreg)
{
	fpuLoadSoftOperand(dst, info, process_flag, fpureg, xmmreg);

	xTEST(dst, 0x7f800000);
	xForwardJNZ8 exponent_nonzero;
	xXOR(dst, dst);
	xForwardJump8 value_ready;
	exponent_nonzero.SetTarget();
	xTEST(dst, 0x80000000);
	xForwardJZ8 value_ready_nonnegative;
	xXOR(dst, 0x7fffffff);
	value_ready_nonnegative.SetTarget();
	value_ready.SetTarget();
}

template <eeOpcode opcode, JccComparisonType condition>
static void recSoftCompare(int info)
{
	EE::Profiler.EmitOp(opcode);
	_freeX86reg(eax.GetId());
	_freeX86reg(ecx.GetId());
	fpuLoadSoftCompareValue(eax, info, PROCESS_EE_S, _Fs_, EEREC_S);
	fpuLoadSoftCompareValue(ecx, info, PROCESS_EE_T, _Ft_, EEREC_T);
	xCMP(eax, ecx);
	xForwardJump8 condition_true(condition);
	xAND(ptr32[&fpuRegs.fprc[31]], ~FPUflagC);
	xForwardJump8 condition_done;
	condition_true.SetTarget();
	xOR(ptr32[&fpuRegs.fprc[31]], FPUflagC);
	condition_done.SetTarget();
}

void recC_EQ_xmm(int info)
{
	EE::Profiler.EmitOp(eeOpcode::CEQ_F);

	//Console.WriteLn("recC_EQ_xmm()");

	switch (info & (PROCESS_EE_S | PROCESS_EE_T))
	{
		case PROCESS_EE_S:
			{
				const int regs = fpuCopyToTempForClamp(_Fs_, EEREC_S);
				fpuFloat3(regs);

				const int t0reg = _allocTempXMMreg(XMMT_FPS);
				xMOVSSZX(xRegisterSSE(t0reg), ptr[&fpuRegs.fpr[_Ft_]]);
				fpuFloat3(t0reg);

				xUCOMI.SS(xRegisterSSE(regs), xRegisterSSE(t0reg));

				_freeXMMreg(t0reg);
				fpuFreeIfTemp(regs);
			}
			break;

		case PROCESS_EE_T:
			{
				const int regt = fpuCopyToTempForClamp(_Ft_, EEREC_T);
				fpuFloat3(regt);

				const int t0reg = _allocTempXMMreg(XMMT_FPS);
				xMOVSSZX(xRegisterSSE(t0reg), ptr[&fpuRegs.fpr[_Fs_]]);
				fpuFloat3(t0reg);

				xUCOMI.SS(xRegisterSSE(t0reg), xRegisterSSE(regt));

				_freeXMMreg(t0reg);
				fpuFreeIfTemp(regt);
			}
			break;

		case (PROCESS_EE_S | PROCESS_EE_T):
			{
				const int regs = fpuCopyToTempForClamp(_Fs_, EEREC_S);
				fpuFloat3(regs);

				const int regt = fpuCopyToTempForClamp(_Ft_, EEREC_T);
				fpuFloat3(regt);

				xUCOMI.SS(xRegisterSSE(regs), xRegisterSSE(regt));

				fpuFreeIfTemp(regs);
				fpuFreeIfTemp(regt);
			}
			break;

		default:
			Console.WriteLn(Color_Magenta, "recC_EQ_xmm: Default");
			xMOV(eax, ptr[&fpuRegs.fpr[_Fs_]]);
			xCMP(eax, ptr[&fpuRegs.fpr[_Ft_]]);

			j8Ptr[0] = JZ8(0);
				xAND(ptr32[&fpuRegs.fprc[31]], ~FPUflagC);
				j8Ptr[1] = JMP8(0);
			x86SetJ8(j8Ptr[0]);
				xOR(ptr32[&fpuRegs.fprc[31]], FPUflagC);
			x86SetJ8(j8Ptr[1]);
			return;
	}

	j8Ptr[0] = JZ8(0);
		xAND(ptr32[&fpuRegs.fprc[31]], ~FPUflagC);
		j8Ptr[1] = JMP8(0);
	x86SetJ8(j8Ptr[0]);
		xOR(ptr32[&fpuRegs.fprc[31]], FPUflagC);
	x86SetJ8(j8Ptr[1]);
}

FPURECOMPILE_CONSTCODE_EXACT(
	C_EQ, XMMINFO_READS | XMMINFO_READT, recSoftCompare<eeOpcode::CEQ_F, Jcc_Equal>);
//REC_FPUFUNC(C_EQ);

void recC_F()
{
	EE::Profiler.EmitOp(eeOpcode::CF_F);
	xAND(ptr32[&fpuRegs.fprc[31]], ~FPUflagC);
}
//REC_FPUFUNC(C_F);

void recC_LE_xmm(int info)
{
	EE::Profiler.EmitOp(eeOpcode::CLE_F);

	//Console.WriteLn("recC_LE_xmm()");

	switch (info & (PROCESS_EE_S | PROCESS_EE_T))
	{
		case PROCESS_EE_S:
		{
			const int regs = fpuCopyToTempForClamp(_Fs_, EEREC_S);
			fpuFloat3(regs);

			const int t0reg = _allocTempXMMreg(XMMT_FPS);
			xMOVSSZX(xRegisterSSE(t0reg), ptr[&fpuRegs.fpr[_Ft_]]);
			fpuFloat3(t0reg);

			xUCOMI.SS(xRegisterSSE(regs), xRegisterSSE(t0reg));

			_freeXMMreg(t0reg);
			fpuFreeIfTemp(regs);
		}
		break;

		case PROCESS_EE_T:
		{
			const int regt = fpuCopyToTempForClamp(_Ft_, EEREC_T);
			fpuFloat3(regt);

			const int t0reg = _allocTempXMMreg(XMMT_FPS);
			xMOVSSZX(xRegisterSSE(t0reg), ptr[&fpuRegs.fpr[_Fs_]]);
			fpuFloat3(t0reg);

			xUCOMI.SS(xRegisterSSE(t0reg), xRegisterSSE(regt));

			_freeXMMreg(t0reg);
			fpuFreeIfTemp(regt);
		}
		break;

		case (PROCESS_EE_S | PROCESS_EE_T):
		{
			const int regs = fpuCopyToTempForClamp(_Fs_, EEREC_S);
			fpuFloat3(regs);

			const int regt = fpuCopyToTempForClamp(_Ft_, EEREC_T);
			fpuFloat3(regt);

			xUCOMI.SS(xRegisterSSE(regs), xRegisterSSE(regt));

			fpuFreeIfTemp(regs);
			fpuFreeIfTemp(regt);
		}
		break;

		default: // Untested and incorrect, but this case is never reached AFAIK (cottonvibes)
			Console.WriteLn(Color_Magenta, "recC_LE_xmm: Default");
			xMOV(eax, ptr[&fpuRegs.fpr[_Fs_]]);
			xCMP(eax, ptr[&fpuRegs.fpr[_Ft_]]);

			j8Ptr[0] = JLE8(0);
				xAND(ptr32[&fpuRegs.fprc[31]], ~FPUflagC);
				j8Ptr[1] = JMP8(0);
			x86SetJ8(j8Ptr[0]);
				xOR(ptr32[&fpuRegs.fprc[31]], FPUflagC);
			x86SetJ8(j8Ptr[1]);
			return;
	}

	j8Ptr[0] = JBE8(0);
		xAND(ptr32[&fpuRegs.fprc[31]], ~FPUflagC);
		j8Ptr[1] = JMP8(0);
	x86SetJ8(j8Ptr[0]);
		xOR(ptr32[&fpuRegs.fprc[31]], FPUflagC);
	x86SetJ8(j8Ptr[1]);
}

FPURECOMPILE_CONSTCODE_EXACT(
	C_LE, XMMINFO_READS | XMMINFO_READT, recSoftCompare<eeOpcode::CLE_F, Jcc_LessOrEqual>);
//REC_FPUFUNC(C_LE);

void recC_LT_xmm(int info)
{
	EE::Profiler.EmitOp(eeOpcode::CLT_F);

	//Console.WriteLn("recC_LT_xmm()");

	switch (info & (PROCESS_EE_S | PROCESS_EE_T))
	{
		case PROCESS_EE_S:
		{
			const int regs = fpuCopyToTempForClamp(_Fs_, EEREC_S);
			fpuFloat3(regs);

			const int t0reg = _allocTempXMMreg(XMMT_FPS);
			xMOVSSZX(xRegisterSSE(t0reg), ptr[&fpuRegs.fpr[_Ft_]]);
			fpuFloat3(t0reg);

			xUCOMI.SS(xRegisterSSE(regs), xRegisterSSE(t0reg));

			_freeXMMreg(t0reg);
			fpuFreeIfTemp(regs);
		}
		break;

		case PROCESS_EE_T:
		{
			const int regt = fpuCopyToTempForClamp(_Ft_, EEREC_T);
			fpuFloat3(regt);

			const int t0reg = _allocTempXMMreg(XMMT_FPS);
			xMOVSSZX(xRegisterSSE(t0reg), ptr[&fpuRegs.fpr[_Fs_]]);
			fpuFloat3(t0reg);

			xUCOMI.SS(xRegisterSSE(t0reg), xRegisterSSE(regt));

			_freeXMMreg(t0reg);
			fpuFreeIfTemp(regt);
		}
		break;

		case (PROCESS_EE_S | PROCESS_EE_T):
		{
			const int regs = fpuCopyToTempForClamp(_Fs_, EEREC_S);
			fpuFloat3(regs);

			const int regt = fpuCopyToTempForClamp(_Ft_, EEREC_T);
			fpuFloat3(regt);

			xUCOMI.SS(xRegisterSSE(regs), xRegisterSSE(regt));

			fpuFreeIfTemp(regs);
			fpuFreeIfTemp(regt);
		}
		break;

		default:
			Console.WriteLn(Color_Magenta, "recC_LT_xmm: Default");
			xMOV(eax, ptr[&fpuRegs.fpr[_Fs_]]);
			xCMP(eax, ptr[&fpuRegs.fpr[_Ft_]]);

			j8Ptr[0] = JL8(0);
				xAND(ptr32[&fpuRegs.fprc[31]], ~FPUflagC);
				j8Ptr[1] = JMP8(0);
			x86SetJ8(j8Ptr[0]);
				xOR(ptr32[&fpuRegs.fprc[31]], FPUflagC);
			x86SetJ8(j8Ptr[1]);
			return;
	}

	j8Ptr[0] = JB8(0);
		xAND(ptr32[&fpuRegs.fprc[31]], ~FPUflagC);
		j8Ptr[1] = JMP8(0);
	x86SetJ8(j8Ptr[0]);
		xOR(ptr32[&fpuRegs.fprc[31]], FPUflagC);
	x86SetJ8(j8Ptr[1]);
}

FPURECOMPILE_CONSTCODE_EXACT(
	C_LT, XMMINFO_READS | XMMINFO_READT, recSoftCompare<eeOpcode::CLT_F, Jcc_Less>);
//REC_FPUFUNC(C_LT);
//------------------------------------------------------------------


//------------------------------------------------------------------
// CVT.x XMM
//------------------------------------------------------------------
void recCVT_S_xmm(int info)
{
	EE::Profiler.EmitOp(eeOpcode::CVTS_F);
	if (info & PROCESS_EE_D)
	{
		if (info & PROCESS_EE_S)
			xCVTDQ2PS(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_S));
		else
			xCVTSI2SS(xRegisterSSE(EEREC_D), ptr32[&fpuRegs.fpr[_Fs_]]);
	}
	else
	{
		const int temp = _allocTempXMMreg(XMMT_FPS);
		xCVTSI2SS(xRegisterSSE(temp), ptr32[&fpuRegs.fpr[_Fs_]]);
		xMOVSS(ptr32[&fpuRegs.fpr[_Fd_]], xRegisterSSE(temp));
		_freeXMMreg(temp);
	}
}

void recCVT_S()
{
	// Float version is fully accurate, no double version
	eeFPURecompileCode(recCVT_S_xmm, R5900::Interpreter::OpcodeImpl::COP1::CVT_S, XMMINFO_WRITED | XMMINFO_READS);
}

void recCVT_W()
{
	// Float version is fully accurate, no double version

	// If we have the following EmitOP() on the top then it'll get calculated twice when CHECK_FPU_FULL is true
	// as we also have an EmitOP() at recCVT_W() on iFPUd.cpp.  hence we have it below the possible return.
	EE::Profiler.EmitOp(eeOpcode::CVTW);

	int regs = _checkXMMreg(XMMTYPE_FPREG, _Fs_, MODE_READ);

	if (regs >= 0)
	{
		xCVTTSS2SI(eax, xRegisterSSE(regs));
		xMOVD(edx, xRegisterSSE(regs));
	}
	else
	{
		xCVTTSS2SI(eax, ptr32[&fpuRegs.fpr[_Fs_]]);
		xMOV(edx, ptr[&fpuRegs.fpr[_Fs_]]);
	}

	//kill register allocation for dst because we write directly to fpuRegs.fpr[_Fd_]
	_deleteFPtoXMMreg(_Fd_, DELETE_REG_FREE_NO_WRITEBACK);

	// cvttss2si converts unrepresentable values to 0x80000000, so negative values are already handled.
	// So we just need to handle positive values.
	xCMP(edx, 0x4f000000); // If the input is greater than INT_MAX
	xMOV(edx, 0x7fffffff);
	xCMOVGE(eax, edx);     // Saturate it

	//Write the result
	xMOV(ptr[&fpuRegs.fpr[_Fd_]], eax);
}
//------------------------------------------------------------------


//------------------------------------------------------------------
// DIV XMM
//------------------------------------------------------------------
void recDIVhelper1(int regd, int regt) // Sets flags
{
	u8 *pjmp1, *pjmp2;
	u32 *ajmp32, *bjmp32;
	const int t1reg = _allocTempXMMreg(XMMT_FPS);

	xAND(ptr32[&fpuRegs.fprc[31]], ~(FPUflagI | FPUflagD)); // Clear I and D flags

	/*--- Check for divide by zero ---*/
	xXOR.PS(xRegisterSSE(t1reg), xRegisterSSE(t1reg));
	xCMPEQ.SS(xRegisterSSE(t1reg), xRegisterSSE(regt));
	xMOVMSKPS(eax, xRegisterSSE(t1reg));
	xAND(eax, 1); //Check sign (if regt == zero, sign will be set)
	ajmp32 = JZ32(0); //Skip if not set

		/*--- Check for 0/0 ---*/
		xXOR.PS(xRegisterSSE(t1reg), xRegisterSSE(t1reg));
		xCMPEQ.SS(xRegisterSSE(t1reg), xRegisterSSE(regd));
		xMOVMSKPS(eax, xRegisterSSE(t1reg));
		xAND(eax, 1); //Check sign (if regd == zero, sign will be set)
		pjmp1 = JZ8(0); //Skip if not set
			xOR(ptr32[&fpuRegs.fprc[31]], FPUflagI | FPUflagSI); // Set I and SI flags ( 0/0 )
			pjmp2 = JMP8(0);
		x86SetJ8(pjmp1); //x/0 but not 0/0
			xOR(ptr32[&fpuRegs.fprc[31]], FPUflagD | FPUflagSD); // Set D and SD flags ( x/0 )
		x86SetJ8(pjmp2);

		/*--- Make regd +/- Maximum ---*/
		xXOR.PS(xRegisterSSE(regd), xRegisterSSE(regt)); // Make regd Positive or Negative
		xAND.PS(xRegisterSSE(regd), ptr[&s_neg[0]]); // Get the sign bit
		xOR.PS(xRegisterSSE(regd), ptr[&g_maxvals[0]]); // regd = +/- Maximum
		//xMOVSSZX(xRegisterSSE(regd), ptr[&g_maxvals[0]]);
		bjmp32 = JMP32(0);

	x86SetJ32(ajmp32);

	/*--- Normal Divide ---*/
	if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat2(regd); fpuFloat2(regt); }
	xDIV.SS(xRegisterSSE(regd), xRegisterSSE(regt));

	ClampValues(regd);
	x86SetJ32(bjmp32);

	_freeXMMreg(t1reg);
}

void recDIVhelper2(int regd, int regt) // Doesn't sets flags
{
	if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat2(regd); fpuFloat2(regt); }
	xDIV.SS(xRegisterSSE(regd), xRegisterSSE(regt));
	ClampValues(regd);
}

alignas(16) static FPControlRegister roundmode_nearest;

void recDIV_S_xmm(int info)
{
	EE::Profiler.EmitOp(eeOpcode::DIV_F);
	int t0reg = _allocTempXMMreg(XMMT_FPS);
	//Console.WriteLn("DIV");

	if (EmuConfig.Cpu.FPUFPCR.bitmask != EmuConfig.Cpu.FPUDivFPCR.bitmask)
		xLDMXCSR(ptr32[&EmuConfig.Cpu.FPUDivFPCR.bitmask]);

	switch (info & (PROCESS_EE_S | PROCESS_EE_T))
	{
		case PROCESS_EE_S:
			//Console.WriteLn("FPU: DIV case 1");
			xMOVSS(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_S));
			xMOVSSZX(xRegisterSSE(t0reg), ptr[&fpuRegs.fpr[_Ft_]]);
			if (CHECK_FPU_EXTRA_FLAGS)
				recDIVhelper1(EEREC_D, t0reg);
			else
				recDIVhelper2(EEREC_D, t0reg);
			break;
		case PROCESS_EE_T:
			//Console.WriteLn("FPU: DIV case 2");
			if (EEREC_D == EEREC_T)
			{
				xMOVSS(xRegisterSSE(t0reg), xRegisterSSE(EEREC_T));
				xMOVSSZX(xRegisterSSE(EEREC_D), ptr[&fpuRegs.fpr[_Fs_]]);
				if (CHECK_FPU_EXTRA_FLAGS)
					recDIVhelper1(EEREC_D, t0reg);
				else
					recDIVhelper2(EEREC_D, t0reg);
			}
			else
			{
				xMOVSSZX(xRegisterSSE(EEREC_D), ptr[&fpuRegs.fpr[_Fs_]]);
				if (CHECK_FPU_EXTRA_FLAGS)
					recDIVhelper1(EEREC_D, EEREC_T);
				else
					recDIVhelper2(EEREC_D, EEREC_T);
			}
			break;
		case (PROCESS_EE_S | PROCESS_EE_T):
			//Console.WriteLn("FPU: DIV case 3");
			if (EEREC_D == EEREC_T)
			{
				xMOVSS(xRegisterSSE(t0reg), xRegisterSSE(EEREC_T));
				xMOVSS(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_S));
				if (CHECK_FPU_EXTRA_FLAGS)
					recDIVhelper1(EEREC_D, t0reg);
				else
					recDIVhelper2(EEREC_D, t0reg);
			}
			else
			{
				xMOVSS(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_S));
				if (CHECK_FPU_EXTRA_FLAGS)
					recDIVhelper1(EEREC_D, EEREC_T);
				else
					recDIVhelper2(EEREC_D, EEREC_T);
			}
			break;
		default:
			//Console.WriteLn("FPU: DIV case 4");
			xMOVSSZX(xRegisterSSE(t0reg), ptr[&fpuRegs.fpr[_Ft_]]);
			xMOVSSZX(xRegisterSSE(EEREC_D), ptr[&fpuRegs.fpr[_Fs_]]);
			if (CHECK_FPU_EXTRA_FLAGS)
				recDIVhelper1(EEREC_D, t0reg);
			else
				recDIVhelper2(EEREC_D, t0reg);
			break;
	}

	if (EmuConfig.Cpu.FPUFPCR.bitmask != EmuConfig.Cpu.FPUDivFPCR.bitmask)
		xLDMXCSR(ptr32[&EmuConfig.Cpu.FPUFPCR.bitmask]);

	_freeXMMreg(t0reg);
}

FPURECOMPILE_CONSTCODE_EXACT(DIV_S, XMMINFO_WRITED | XMMINFO_READS | XMMINFO_READT, recSoftDiv);
//------------------------------------------------------------------



//------------------------------------------------------------------
// MADD XMM
//------------------------------------------------------------------
void recMADDtemp(int info, int regd)
{
	const int t0reg = _allocTempXMMreg(XMMT_FPS);

	switch (info & (PROCESS_EE_S | PROCESS_EE_T))
	{
		case PROCESS_EE_S:
			if (regd == EEREC_S)
			{
				xMOVSSZX(xRegisterSSE(t0reg), ptr[&fpuRegs.fpr[_Ft_]]);
				if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat2(regd); fpuFloat2(t0reg); }
				xMUL.SS(xRegisterSSE(regd), xRegisterSSE(t0reg));
				if (info & PROCESS_EE_ACC)
				{
					if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat(EEREC_ACC); fpuFloat(regd); }
					FPU_ADD(regd, EEREC_ACC);
				}
				else
				{
					xMOVSSZX(xRegisterSSE(t0reg), ptr[&fpuRegs.ACC]);
					if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat(EEREC_ACC); fpuFloat(t0reg); }
					FPU_ADD(regd, t0reg);
				}
			}
			else if ((info & PROCESS_EE_ACC) && regd == EEREC_ACC)
			{
				xMOVSSZX(xRegisterSSE(t0reg), ptr[&fpuRegs.fpr[_Ft_]]);
				if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat2(EEREC_S); fpuFloat2(t0reg); }
				xMUL.SS(xRegisterSSE(t0reg), xRegisterSSE(EEREC_S));
				if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat(regd); fpuFloat(t0reg); }
				FPU_ADD(regd, t0reg);
			}
			else
			{
				xMOVSSZX(xRegisterSSE(regd), ptr[&fpuRegs.fpr[_Ft_]]);
				if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat2(regd); fpuFloat2(EEREC_S); }
				xMUL.SS(xRegisterSSE(regd), xRegisterSSE(EEREC_S));
				if (info & PROCESS_EE_ACC)
				{
					if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat(EEREC_ACC); fpuFloat(regd); }
					FPU_ADD(regd, EEREC_ACC);
				}
				else
				{
					xMOVSSZX(xRegisterSSE(t0reg), ptr[&fpuRegs.ACC]);
					if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat(EEREC_ACC); fpuFloat(t0reg); }
					FPU_ADD(regd, t0reg);
				}
			}
			break;
		case PROCESS_EE_T:
			if (regd == EEREC_T)
			{
				xMOVSSZX(xRegisterSSE(t0reg), ptr[&fpuRegs.fpr[_Fs_]]);
				if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat2(regd); fpuFloat2(t0reg); }
				xMUL.SS(xRegisterSSE(regd), xRegisterSSE(t0reg));
				if (info & PROCESS_EE_ACC)
				{
					if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat(EEREC_ACC); fpuFloat(regd); }
					FPU_ADD(regd, EEREC_ACC);
				}
				else
				{
					xMOVSSZX(xRegisterSSE(t0reg), ptr[&fpuRegs.ACC]);
					if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat(EEREC_ACC); fpuFloat(t0reg); }
					FPU_ADD(regd, t0reg);
				}
			}
			else if ((info & PROCESS_EE_ACC) && regd == EEREC_ACC)
			{
				xMOVSSZX(xRegisterSSE(t0reg), ptr[&fpuRegs.fpr[_Fs_]]);
				if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat2(EEREC_T); fpuFloat2(t0reg); }
				xMUL.SS(xRegisterSSE(t0reg), xRegisterSSE(EEREC_T));
				if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat(regd); fpuFloat(t0reg); }
				FPU_ADD(regd, t0reg);
			}
			else
			{
				xMOVSSZX(xRegisterSSE(regd), ptr[&fpuRegs.fpr[_Fs_]]);
				if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat2(regd); fpuFloat2(EEREC_T); }
				xMUL.SS(xRegisterSSE(regd), xRegisterSSE(EEREC_T));
				if (info & PROCESS_EE_ACC)
				{
					if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat(EEREC_ACC); fpuFloat(regd); }
					FPU_ADD(regd, EEREC_ACC);
				}
				else
				{
					xMOVSSZX(xRegisterSSE(t0reg), ptr[&fpuRegs.ACC]);
					if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat(EEREC_ACC); fpuFloat(t0reg); }
					FPU_ADD(regd, t0reg);
				}
			}
			break;
		case (PROCESS_EE_S | PROCESS_EE_T):
			if (regd == EEREC_S)
			{
				if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat2(regd); fpuFloat2(EEREC_T); }
				xMUL.SS(xRegisterSSE(regd), xRegisterSSE(EEREC_T));
				if (info & PROCESS_EE_ACC)
				{
					if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat(regd); fpuFloat(EEREC_ACC); }
					FPU_ADD(regd, EEREC_ACC);
				}
				else
				{
					xMOVSSZX(xRegisterSSE(t0reg), ptr[&fpuRegs.ACC]);
					if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat(regd); fpuFloat(t0reg); }
					FPU_ADD(regd, t0reg);
				}
			}
			else if (regd == EEREC_T)
			{
				if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat2(regd); fpuFloat2(EEREC_S); }
				xMUL.SS(xRegisterSSE(regd), xRegisterSSE(EEREC_S));
				if (info & PROCESS_EE_ACC)
				{
					if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat(regd); fpuFloat(EEREC_ACC); }
					FPU_ADD(regd, EEREC_ACC);
				}
				else
				{
					xMOVSSZX(xRegisterSSE(t0reg), ptr[&fpuRegs.ACC]);
					if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat(regd); fpuFloat(t0reg); }
					FPU_ADD(regd, t0reg);
				}
			}
			else if ((info & PROCESS_EE_ACC) && regd == EEREC_ACC)
			{
				xMOVSS(xRegisterSSE(t0reg), xRegisterSSE(EEREC_S));
				if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat2(t0reg); fpuFloat2(EEREC_T); }
				xMUL.SS(xRegisterSSE(t0reg), xRegisterSSE(EEREC_T));
				if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat(regd); fpuFloat(t0reg); }
				FPU_ADD(regd, t0reg);
			}
			else
			{
				xMOVSS(xRegisterSSE(regd), xRegisterSSE(EEREC_S));
				if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat2(regd); fpuFloat2(EEREC_T); }
				xMUL.SS(xRegisterSSE(regd), xRegisterSSE(EEREC_T));
				if (info & PROCESS_EE_ACC)
				{
					if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat(regd); fpuFloat(EEREC_ACC); }
					FPU_ADD(regd, EEREC_ACC);
				}
				else
				{
					xMOVSSZX(xRegisterSSE(t0reg), ptr[&fpuRegs.ACC]);
					if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat(regd); fpuFloat(t0reg); }
					FPU_ADD(regd, t0reg);
				}
			}
			break;
		default:
			if ((info & PROCESS_EE_ACC) && regd == EEREC_ACC)
			{
				const int t1reg = _allocTempXMMreg(XMMT_FPS);
				xMOVSSZX(xRegisterSSE(t0reg), ptr[&fpuRegs.fpr[_Fs_]]);
				xMOVSSZX(xRegisterSSE(t1reg), ptr[&fpuRegs.fpr[_Ft_]]);
				if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat2(t0reg); fpuFloat2(t1reg); }
				xMUL.SS(xRegisterSSE(t0reg), xRegisterSSE(t1reg));
				if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat(regd); fpuFloat(t0reg); }
				FPU_ADD(regd, t0reg);
				_freeXMMreg(t1reg);
			}
			else
			{
				xMOVSSZX(xRegisterSSE(regd), ptr[&fpuRegs.fpr[_Fs_]]);
				xMOVSSZX(xRegisterSSE(t0reg), ptr[&fpuRegs.fpr[_Ft_]]);
				if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat2(regd); fpuFloat2(t0reg); }
				xMUL.SS(xRegisterSSE(regd), xRegisterSSE(t0reg));
				if (info & PROCESS_EE_ACC)
				{
					if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat(regd); fpuFloat(EEREC_ACC); }
					FPU_ADD(regd, EEREC_ACC);
				}
				else
				{
					xMOVSSZX(xRegisterSSE(t0reg), ptr[&fpuRegs.ACC]);
					if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat(regd); fpuFloat(t0reg); }
					FPU_ADD(regd, t0reg);
				}
			}
			break;
	}

	if (regd == EEREC_ACC)
		fpuUpdateNativeAccOverflow(regd);
	ClampValues(regd);
	_freeXMMreg(t0reg);
}

void recMADD_S_xmm(int info)
{
	EE::Profiler.EmitOp(eeOpcode::MADD_F);
	//xAND(ptr32[&fpuRegs.fprc[31]], ~(FPUflagO|FPUflagU)); // Clear O and U flags
	recMADDtemp(info, EEREC_D);
}

FPURECOMPILE_CONSTCODE_EXACT(MADD_S,
	XMMINFO_WRITED | XMMINFO_READACC | XMMINFO_READS | XMMINFO_READT,
	recSoftMadd<eeOpcode::MADD_F, false, false>);

void recMADDA_S_xmm(int info)
{
	EE::Profiler.EmitOp(eeOpcode::MADDA_F);
	//xAND(ptr32[&fpuRegs.fprc[31]], ~(FPUflagO|FPUflagU)); // Clear O and U flags
	recMADDtemp(info, EEREC_ACC);
}

FPURECOMPILE_CONSTCODE_EXACT(MADDA_S,
	XMMINFO_WRITEACC | XMMINFO_READACC | XMMINFO_READS | XMMINFO_READT,
	recSoftMadd<eeOpcode::MADDA_F, false, true>);
//------------------------------------------------------------------


//------------------------------------------------------------------
// MAX / MIN XMM
//------------------------------------------------------------------
void recMAX_S_xmm(int info)
{
	EE::Profiler.EmitOp(eeOpcode::MAX_F);
	//xAND(ptr32[&fpuRegs.fprc[31]], ~(FPUflagO|FPUflagU)); // Clear O and U flags
	recCommutativeOp(info, EEREC_D, 2);
}

FPURECOMPILE_CONSTCODE(MAX_S, XMMINFO_WRITED | XMMINFO_READS | XMMINFO_READT);

void recMIN_S_xmm(int info)
{
	EE::Profiler.EmitOp(eeOpcode::MIN_F);
	//xAND(ptr32[&fpuRegs.fprc[31]], ~(FPUflagO|FPUflagU)); // Clear O and U flags
	recCommutativeOp(info, EEREC_D, 3);
}

FPURECOMPILE_CONSTCODE(MIN_S, XMMINFO_WRITED | XMMINFO_READS | XMMINFO_READT);
//------------------------------------------------------------------


//------------------------------------------------------------------
// MOV XMM
//------------------------------------------------------------------
void recMOV_S_xmm(int info)
{
	EE::Profiler.EmitOp(eeOpcode::MOV_F);
	if (info & PROCESS_EE_S)
		xMOVSS(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_S));
	else
		xMOVSSZX(xRegisterSSE(EEREC_D), ptr[&fpuRegs.fpr[_Fs_]]);
}

FPURECOMPILE_CONSTCODE(MOV_S, XMMINFO_WRITED | XMMINFO_READS);
//------------------------------------------------------------------


//------------------------------------------------------------------
// MSUB XMM
//------------------------------------------------------------------
void recMSUBtemp(int info, int regd)
{
	int t0reg = _allocTempXMMreg(XMMT_FPS);

	switch (info & (PROCESS_EE_S | PROCESS_EE_T))
	{
		case PROCESS_EE_S:
			if (regd == EEREC_S)
			{
				xMOVSSZX(xRegisterSSE(t0reg), ptr[&fpuRegs.fpr[_Ft_]]);
				if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat2(regd); fpuFloat2(t0reg); }
				xMUL.SS(xRegisterSSE(regd), xRegisterSSE(t0reg));
				if (info & PROCESS_EE_ACC)
					xMOVSS(xRegisterSSE(t0reg), xRegisterSSE(EEREC_ACC));
				else
					xMOVSSZX(xRegisterSSE(t0reg), ptr[&fpuRegs.ACC]);
				if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat(regd); fpuFloat(t0reg); }
				FPU_SUB(t0reg, regd);
				xMOVSS(xRegisterSSE(regd), xRegisterSSE(t0reg));
			}
			else if ((info & PROCESS_EE_ACC) && regd == EEREC_ACC)
			{
				xMOVSSZX(xRegisterSSE(t0reg), ptr[&fpuRegs.fpr[_Ft_]]);
				if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat2(EEREC_S); fpuFloat2(t0reg); }
				xMUL.SS(xRegisterSSE(t0reg), xRegisterSSE(EEREC_S));
				if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat(regd); fpuFloat(t0reg); }
				FPU_SUB(regd, t0reg);
			}
			else
			{
				xMOVSSZX(xRegisterSSE(regd), ptr[&fpuRegs.fpr[_Ft_]]);
				if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat2(regd); fpuFloat2(EEREC_S); }
				xMUL.SS(xRegisterSSE(regd), xRegisterSSE(EEREC_S));
				if (info & PROCESS_EE_ACC)
					xMOVSS(xRegisterSSE(t0reg), xRegisterSSE(EEREC_ACC));
				else
					xMOVSSZX(xRegisterSSE(t0reg), ptr[&fpuRegs.ACC]);
				if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat(regd); fpuFloat(t0reg); }
				FPU_SUB(t0reg, regd);
				xMOVSS(xRegisterSSE(regd), xRegisterSSE(t0reg));
			}
			break;
		case PROCESS_EE_T:
			if (regd == EEREC_T)
			{
				xMOVSSZX(xRegisterSSE(t0reg), ptr[&fpuRegs.fpr[_Fs_]]);
				if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat2(regd); fpuFloat2(t0reg); }
				xMUL.SS(xRegisterSSE(regd), xRegisterSSE(t0reg));
				if (info & PROCESS_EE_ACC)
					xMOVSS(xRegisterSSE(t0reg), xRegisterSSE(EEREC_ACC));
				else
					xMOVSSZX(xRegisterSSE(t0reg), ptr[&fpuRegs.ACC]);
				if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat(regd); fpuFloat(t0reg); }
				FPU_SUB(t0reg, regd);
				xMOVSS(xRegisterSSE(regd), xRegisterSSE(t0reg));
			}
			else if ((info & PROCESS_EE_ACC) && regd == EEREC_ACC)
			{
				xMOVSSZX(xRegisterSSE(t0reg), ptr[&fpuRegs.fpr[_Fs_]]);
				if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat2(EEREC_T); fpuFloat2(t0reg); }
				xMUL.SS(xRegisterSSE(t0reg), xRegisterSSE(EEREC_T));
				if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat(regd); fpuFloat(t0reg); }
				FPU_SUB(regd, t0reg);
			}
			else
			{
				xMOVSSZX(xRegisterSSE(regd), ptr[&fpuRegs.fpr[_Fs_]]);
				if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat2(regd); fpuFloat2(EEREC_T); }
				xMUL.SS(xRegisterSSE(regd), xRegisterSSE(EEREC_T));
				if (info & PROCESS_EE_ACC)
					xMOVSS(xRegisterSSE(t0reg), xRegisterSSE(EEREC_ACC));
				else
					xMOVSSZX(xRegisterSSE(t0reg), ptr[&fpuRegs.ACC]);
				if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat(regd); fpuFloat(t0reg); }
				FPU_SUB(t0reg, regd);
				xMOVSS(xRegisterSSE(regd), xRegisterSSE(t0reg));
			}
			break;
		case (PROCESS_EE_S | PROCESS_EE_T):
			if (regd == EEREC_S)
			{
				if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat2(regd); fpuFloat2(EEREC_T); }
				xMUL.SS(xRegisterSSE(regd), xRegisterSSE(EEREC_T));
				if (info & PROCESS_EE_ACC)
					xMOVSS(xRegisterSSE(t0reg), xRegisterSSE(EEREC_ACC));
				else
					xMOVSSZX(xRegisterSSE(t0reg), ptr[&fpuRegs.ACC]);
				if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat(regd); fpuFloat(t0reg); }
				FPU_SUB(t0reg, regd);
				xMOVSS(xRegisterSSE(regd), xRegisterSSE(t0reg));
			}
			else if (regd == EEREC_T)
			{
				if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat2(regd); fpuFloat2(EEREC_S); }
				xMUL.SS(xRegisterSSE(regd), xRegisterSSE(EEREC_S));
				if (info & PROCESS_EE_ACC)
					xMOVSS(xRegisterSSE(t0reg), xRegisterSSE(EEREC_ACC));
				else
					xMOVSSZX(xRegisterSSE(t0reg), ptr[&fpuRegs.ACC]);
				if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat(regd); fpuFloat(t0reg); }
				FPU_SUB(t0reg, regd);
				xMOVSS(xRegisterSSE(regd), xRegisterSSE(t0reg));
			}
			else if ((info & PROCESS_EE_ACC) && regd == EEREC_ACC)
			{
				xMOVSS(xRegisterSSE(t0reg), xRegisterSSE(EEREC_S));
				if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat2(t0reg); fpuFloat2(EEREC_T); }
				xMUL.SS(xRegisterSSE(t0reg), xRegisterSSE(EEREC_T));
				if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat(regd); fpuFloat(t0reg); }
				FPU_SUB(regd, t0reg);
			}
			else
			{
				xMOVSS(xRegisterSSE(regd), xRegisterSSE(EEREC_S));
				if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat2(regd); fpuFloat2(EEREC_T); }
				xMUL.SS(xRegisterSSE(regd), xRegisterSSE(EEREC_T));
				if (info & PROCESS_EE_ACC)
					xMOVSS(xRegisterSSE(t0reg), xRegisterSSE(EEREC_ACC));
				else
					xMOVSSZX(xRegisterSSE(t0reg), ptr[&fpuRegs.ACC]);
				if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat(regd); fpuFloat(t0reg); }
				FPU_SUB(t0reg, regd);
				xMOVSS(xRegisterSSE(regd), xRegisterSSE(t0reg));
			}
			break;
		default:
			if ((info & PROCESS_EE_ACC) && regd == EEREC_ACC)
			{
				const int t1reg = _allocTempXMMreg(XMMT_FPS);
				xMOVSSZX(xRegisterSSE(t0reg), ptr[&fpuRegs.fpr[_Fs_]]);
				xMOVSSZX(xRegisterSSE(t1reg), ptr[&fpuRegs.fpr[_Ft_]]);
				if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat2(t0reg); fpuFloat2(t1reg); }
				xMUL.SS(xRegisterSSE(t0reg), xRegisterSSE(t1reg));
				if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat(regd); fpuFloat(t0reg); }
				FPU_SUB(regd, t0reg);
				_freeXMMreg(t1reg);
			}
			else
			{
				xMOVSSZX(xRegisterSSE(regd), ptr[&fpuRegs.fpr[_Fs_]]);
				xMOVSSZX(xRegisterSSE(t0reg), ptr[&fpuRegs.fpr[_Ft_]]);
				if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat2(regd); fpuFloat2(t0reg); }
				xMUL.SS(xRegisterSSE(regd), xRegisterSSE(t0reg));
				if (info & PROCESS_EE_ACC)
					xMOVSS(xRegisterSSE(t0reg), xRegisterSSE(EEREC_ACC));
				else
					xMOVSSZX(xRegisterSSE(t0reg), ptr[&fpuRegs.ACC]);
				if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat(regd); fpuFloat(t0reg); }
				FPU_SUB(t0reg, regd);
				xMOVSS(xRegisterSSE(regd), xRegisterSSE(t0reg));
			}
			break;
	}

	if (regd == EEREC_ACC)
		fpuUpdateNativeAccOverflow(regd);
	ClampValues(regd);
	_freeXMMreg(t0reg);
}

void recMSUB_S_xmm(int info)
{
	EE::Profiler.EmitOp(eeOpcode::MSUB_F);
	//xAND(ptr32[&fpuRegs.fprc[31]], ~(FPUflagO|FPUflagU)); // Clear O and U flags
	recMSUBtemp(info, EEREC_D);
}

FPURECOMPILE_CONSTCODE_EXACT(MSUB_S,
	XMMINFO_WRITED | XMMINFO_READACC | XMMINFO_READS | XMMINFO_READT,
	recSoftMadd<eeOpcode::MSUB_F, true, false>);

void recMSUBA_S_xmm(int info)
{
	EE::Profiler.EmitOp(eeOpcode::MSUBA_F);
	//xAND(ptr32[&fpuRegs.fprc[31]], ~(FPUflagO|FPUflagU)); // Clear O and U flags
	recMSUBtemp(info, EEREC_ACC);
}

FPURECOMPILE_CONSTCODE_EXACT(MSUBA_S,
	XMMINFO_WRITEACC | XMMINFO_READACC | XMMINFO_READS | XMMINFO_READT,
	recSoftMadd<eeOpcode::MSUBA_F, true, true>);
//------------------------------------------------------------------


//------------------------------------------------------------------
// MUL XMM
//------------------------------------------------------------------
void recMUL_S_xmm(int info)
{
	EE::Profiler.EmitOp(eeOpcode::MUL_F);
	//xAND(ptr32[&fpuRegs.fprc[31]], ~(FPUflagO|FPUflagU)); // Clear O and U flags
	ClampValues(recCommutativeOp(info, EEREC_D, 1));
}

FPURECOMPILE_CONSTCODE_EXACT(
	MUL_S, XMMINFO_WRITED | XMMINFO_READS | XMMINFO_READT, recSoftMul<eeOpcode::MUL_F, false>);

void recMULA_S_xmm(int info)
{
	EE::Profiler.EmitOp(eeOpcode::MULA_F);
	//xAND(ptr32[&fpuRegs.fprc[31]], ~(FPUflagO|FPUflagU)); // Clear O and U flags
	const int regd = recCommutativeOp(info, EEREC_ACC, 1);
	fpuUpdateNativeAccOverflow(regd);
	ClampValues(regd);
}

FPURECOMPILE_CONSTCODE_EXACT(
	MULA_S, XMMINFO_WRITEACC | XMMINFO_READS | XMMINFO_READT, recSoftMul<eeOpcode::MULA_F, true>);
//------------------------------------------------------------------


//------------------------------------------------------------------
// NEG XMM
//------------------------------------------------------------------
void recNEG_S_xmm(int info)
{
	EE::Profiler.EmitOp(eeOpcode::NEG_F);
	if (info & PROCESS_EE_S)
		xMOVSS(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_S));
	else
		xMOVSSZX(xRegisterSSE(EEREC_D), ptr[&fpuRegs.fpr[_Fs_]]);

	//xAND(ptr32[&fpuRegs.fprc[31]], ~(FPUflagO|FPUflagU)); // Clear O and U flags
	xXOR.PS(xRegisterSSE(EEREC_D), ptr[&s_neg[0]]);

	// Always preserve sign. Using float clamping here would result in
	// +inf to become +fMax instead of -fMax, which is definitely wrong.
	fpuFloat3(EEREC_D);
}

FPURECOMPILE_CONSTCODE(NEG_S, XMMINFO_WRITED | XMMINFO_READS);
//------------------------------------------------------------------


//------------------------------------------------------------------
// SUB XMM
//------------------------------------------------------------------
void recSUBhelper(int regd, int regt)
{
	if (CHECK_FPU_EXTRA_OVERFLOW /*&& !CHECK_FPUCLAMPHACK*/) { fpuFloat2(regd); fpuFloat2(regt); }
	FPU_SUB(regd, regt);
}

void recSUBop(int info, int regd)
{
	int t0reg = _allocTempXMMreg(XMMT_FPS);

	//xAND(ptr32[&fpuRegs.fprc[31]], ~(FPUflagO|FPUflagU)); // Clear O and U flags

	switch (info & (PROCESS_EE_S | PROCESS_EE_T))
	{
		case PROCESS_EE_S:
			//Console.WriteLn("FPU: SUB case 1");
			if (regd != EEREC_S)
				xMOVSS(xRegisterSSE(regd), xRegisterSSE(EEREC_S));
			xMOVSSZX(xRegisterSSE(t0reg), ptr[&fpuRegs.fpr[_Ft_]]);
			recSUBhelper(regd, t0reg);
			break;
		case PROCESS_EE_T:
			//Console.WriteLn("FPU: SUB case 2");
			if (regd == EEREC_T)
			{
				xMOVSS(xRegisterSSE(t0reg), xRegisterSSE(EEREC_T));
				xMOVSSZX(xRegisterSSE(regd), ptr[&fpuRegs.fpr[_Fs_]]);
				recSUBhelper(regd, t0reg);
			}
			else
			{
				xMOVSSZX(xRegisterSSE(regd), ptr[&fpuRegs.fpr[_Fs_]]);
				recSUBhelper(regd, EEREC_T);
			}
			break;
		case (PROCESS_EE_S | PROCESS_EE_T):
			//Console.WriteLn("FPU: SUB case 3");
			if (regd == EEREC_T)
			{
				xMOVSS(xRegisterSSE(t0reg), xRegisterSSE(EEREC_T));
				xMOVSS(xRegisterSSE(regd), xRegisterSSE(EEREC_S));
				recSUBhelper(regd, t0reg);
			}
			else
			{
				xMOVSS(xRegisterSSE(regd), xRegisterSSE(EEREC_S));
				recSUBhelper(regd, EEREC_T);
			}
			break;
		default:
			Console.Warning("FPU: SUB case 4");
			xMOVSSZX(xRegisterSSE(t0reg), ptr[&fpuRegs.fpr[_Ft_]]);
			xMOVSSZX(xRegisterSSE(regd), ptr[&fpuRegs.fpr[_Fs_]]);
			recSUBhelper(regd, t0reg);
			break;
	}

	ClampValues(regd);
	_freeXMMreg(t0reg);
}

void recSUB_S_xmm(int info)
{
	EE::Profiler.EmitOp(eeOpcode::SUB_F);
	recSUBop(info, EEREC_D);
}

FPURECOMPILE_CONSTCODE_EXACT(
	SUB_S, XMMINFO_WRITED | XMMINFO_READS | XMMINFO_READT, recSoftAddSub<eeOpcode::SUB_F, true, false>);


void recSUBA_S_xmm(int info)
{
	EE::Profiler.EmitOp(eeOpcode::SUBA_F);
	recSUBop(info, EEREC_ACC);
}

FPURECOMPILE_CONSTCODE_EXACT(
	SUBA_S, XMMINFO_WRITEACC | XMMINFO_READS | XMMINFO_READT, recSoftAddSub<eeOpcode::SUBA_F, true, true>);
//------------------------------------------------------------------


//------------------------------------------------------------------
// SQRT XMM
//------------------------------------------------------------------
void recSQRT_S_xmm(int info)
{
	EE::Profiler.EmitOp(eeOpcode::SQRT_F);
	bool roundmodeFlag = false;
	//Console.WriteLn("FPU: SQRT");

	if (EmuConfig.Cpu.FPUFPCR.GetRoundMode() != FPRoundMode::Nearest)
	{
		// Set roundmode to nearest if it isn't already
		//Console.WriteLn("sqrt to nearest");
		roundmode_nearest = EmuConfig.Cpu.FPUFPCR;
		roundmode_nearest.SetRoundMode(FPRoundMode::Nearest);
		xLDMXCSR(ptr32[&roundmode_nearest.bitmask]);
		roundmodeFlag = true;
	}

	if (info & PROCESS_EE_T)
		xMOVSS(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_T));
	else
		xMOVSSZX(xRegisterSSE(EEREC_D), ptr[&fpuRegs.fpr[_Ft_]]);

	if (CHECK_FPU_EXTRA_FLAGS)
	{
		xAND(ptr32[&fpuRegs.fprc[31]], ~(FPUflagI | FPUflagD)); // Clear I and D flags

		/*--- Check for negative SQRT ---*/
		xMOVMSKPS(eax, xRegisterSSE(EEREC_D));
		xAND(eax, 1); //Check sign
		u8* pjmp = JZ8(0); //Skip if none are
			xOR(ptr32[&fpuRegs.fprc[31]], FPUflagI | FPUflagSI); // Set I and SI flags
			xAND.PS(xRegisterSSE(EEREC_D), ptr[&s_pos[0]]); // Make EEREC_D Positive
		x86SetJ8(pjmp);
	}
	else
		xAND.PS(xRegisterSSE(EEREC_D), ptr[&s_pos[0]]); // Make EEREC_D Positive

	if (CHECK_FPU_OVERFLOW) // Only need to do positive clamp, since EEREC_D is positive
		xMIN.SS(xRegisterSSE(EEREC_D), ptr[&g_maxvals[0]]);
	xSQRT.SS(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_D));
	if (CHECK_FPU_EXTRA_OVERFLOW) // Shouldn't need to clamp again since SQRT of a number will always be smaller than the original number, doing it just incase :/
		ClampValues(EEREC_D);

	if (roundmodeFlag)
		xLDMXCSR(ptr32[&EmuConfig.Cpu.FPUFPCR.bitmask]);
}

FPURECOMPILE_CONSTCODE_EXACT(SQRT_S, XMMINFO_WRITED | XMMINFO_READT, recSoftSqrt);
//------------------------------------------------------------------


//------------------------------------------------------------------
// RSQRT XMM
//------------------------------------------------------------------
void recRSQRThelper1(int regd, int t0reg) // Preforms the RSQRT function when regd <- Fs and t0reg <- Ft (Sets correct flags)
{
	u8 *pjmp1, *pjmp2;
	u32 *pjmp32;
	u8 *qjmp1, *qjmp2;
	int t1reg = _allocTempXMMreg(XMMT_FPS);

	xAND(ptr32[&fpuRegs.fprc[31]], ~(FPUflagI | FPUflagD)); // Clear I and D flags

	/*--- (first) Check for negative SQRT ---*/
	xMOVMSKPS(eax, xRegisterSSE(t0reg));
	xAND(eax, 1); //Check sign
	pjmp2 = JZ8(0); //Skip if not set
		xOR(ptr32[&fpuRegs.fprc[31]], FPUflagI | FPUflagSI); // Set I and SI flags
		xAND.PS(xRegisterSSE(t0reg), ptr[&s_pos[0]]); // Make t0reg Positive
	x86SetJ8(pjmp2);

	/*--- Check for zero ---*/
	xXOR.PS(xRegisterSSE(t1reg), xRegisterSSE(t1reg));
	xCMPEQ.SS(xRegisterSSE(t1reg), xRegisterSSE(t0reg));
	xMOVMSKPS(eax, xRegisterSSE(t1reg));
	xAND(eax, 1); //Check sign (if t0reg == zero, sign will be set)
	pjmp1 = JZ8(0); //Skip if not set
		/*--- Check for 0/0 ---*/
		xXOR.PS(xRegisterSSE(t1reg), xRegisterSSE(t1reg));
		xCMPEQ.SS(xRegisterSSE(t1reg), xRegisterSSE(regd));
		xMOVMSKPS(eax, xRegisterSSE(t1reg));
		xAND(eax, 1); //Check sign (if regd == zero, sign will be set)
		qjmp1 = JZ8(0); //Skip if not set
			xOR(ptr32[&fpuRegs.fprc[31]], FPUflagI | FPUflagSI); // Set I and SI flags ( 0/0 )
			qjmp2 = JMP8(0);
		x86SetJ8(qjmp1); //x/0 but not 0/0
			xOR(ptr32[&fpuRegs.fprc[31]], FPUflagD | FPUflagSD); // Set D and SD flags ( x/0 )
		x86SetJ8(qjmp2);

		/*--- Make regd +/- Maximum ---*/
		xAND.PS(xRegisterSSE(regd), ptr[&s_neg[0]]); // Get the sign bit
		xOR.PS(xRegisterSSE(regd), ptr[&g_maxvals[0]]); // regd = +/- Maximum
		pjmp32 = JMP32(0);
	x86SetJ8(pjmp1);

	if (CHECK_FPU_EXTRA_OVERFLOW)
	{
		xMIN.SS(xRegisterSSE(t0reg), ptr[&g_maxvals[0]]); // Only need to do positive clamp, since t0reg is positive
		fpuFloat2(regd);
	}

	xSQRT.SS(xRegisterSSE(t0reg), xRegisterSSE(t0reg));
	xDIV.SS(xRegisterSSE(regd), xRegisterSSE(t0reg));

	ClampValues(regd);
	x86SetJ32(pjmp32);

	_freeXMMreg(t1reg);
}

void recRSQRThelper2(int regd, int t0reg) // Preforms the RSQRT function when regd <- Fs and t0reg <- Ft (Doesn't set flags)
{
	xAND.PS(xRegisterSSE(t0reg), ptr[&s_pos[0]]); // Make t0reg Positive
	if (CHECK_FPU_EXTRA_OVERFLOW)
	{
		xMIN.SS(xRegisterSSE(t0reg), ptr[&g_maxvals[0]]); // Only need to do positive clamp, since t0reg is positive
		fpuFloat2(regd);
	}
	xSQRT.SS(xRegisterSSE(t0reg), xRegisterSSE(t0reg));
	xDIV.SS(xRegisterSSE(regd), xRegisterSSE(t0reg));
	ClampValues(regd);
}

void recRSQRT_S_xmm(int info)
{
	EE::Profiler.EmitOp(eeOpcode::RSQRT_F);

	// RSQRT doesn't change the round mode, because RSQRTSS ignores the rounding mode in MXCSR.
	const int t0reg = _allocTempXMMreg(XMMT_FPS);
	//Console.WriteLn("FPU: RSQRT");

	switch (info & (PROCESS_EE_S | PROCESS_EE_T))
	{
		case PROCESS_EE_S:
			//Console.WriteLn("FPU: RSQRT case 1");
			xMOVSS(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_S));
			xMOVSSZX(xRegisterSSE(t0reg), ptr[&fpuRegs.fpr[_Ft_]]);
			if (CHECK_FPU_EXTRA_FLAGS)
				recRSQRThelper1(EEREC_D, t0reg);
			else
				recRSQRThelper2(EEREC_D, t0reg);
			break;
		case PROCESS_EE_T:
			//Console.WriteLn("FPU: RSQRT case 2");
			xMOVSS(xRegisterSSE(t0reg), xRegisterSSE(EEREC_T));
			xMOVSSZX(xRegisterSSE(EEREC_D), ptr[&fpuRegs.fpr[_Fs_]]);
			if (CHECK_FPU_EXTRA_FLAGS)
				recRSQRThelper1(EEREC_D, t0reg);
			else
				recRSQRThelper2(EEREC_D, t0reg);
			break;
		case (PROCESS_EE_S | PROCESS_EE_T):
			//Console.WriteLn("FPU: RSQRT case 3");
			xMOVSS(xRegisterSSE(t0reg), xRegisterSSE(EEREC_T));
			xMOVSS(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_S));
			if (CHECK_FPU_EXTRA_FLAGS)
				recRSQRThelper1(EEREC_D, t0reg);
			else
				recRSQRThelper2(EEREC_D, t0reg);
			break;
		default:
			//Console.WriteLn("FPU: RSQRT case 4");
			xMOVSSZX(xRegisterSSE(t0reg), ptr[&fpuRegs.fpr[_Ft_]]);
			xMOVSSZX(xRegisterSSE(EEREC_D), ptr[&fpuRegs.fpr[_Fs_]]);
			if (CHECK_FPU_EXTRA_FLAGS)
				recRSQRThelper1(EEREC_D, t0reg);
			else
				recRSQRThelper2(EEREC_D, t0reg);
			break;
	}
	_freeXMMreg(t0reg);
}

FPURECOMPILE_CONSTCODE_EXACT(RSQRT_S, XMMINFO_WRITED | XMMINFO_READS | XMMINFO_READT, recSoftRsqrt);

#endif // FPU_RECOMPILE

} // namespace COP1
} // namespace OpcodeImpl
} // namespace Dynarec
} // namespace R5900
