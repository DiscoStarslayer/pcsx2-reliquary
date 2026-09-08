// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

static void mVUemitUpperSoftStackAlloc(int size)
{
	xSUB(rsp, size);
}

static void mVUemitUpperSoftStackFree(int size)
{
	xADD(rsp, size);
}

static constexpr int VU_SOFT_ADD_REPAIR_SOURCE = (sizeof(VuSoftFmacJitResult) + 15) & ~15;
static constexpr int VU_SOFT_ADD_REPAIR_OPERAND = VU_SOFT_ADD_REPAIR_SOURCE + 16;
static constexpr int VU_SOFT_ADD_REPAIR_BAD_MASK = VU_SOFT_ADD_REPAIR_OPERAND + 16;
static constexpr int VU_SOFT_ADD_REPAIR_ACTIVE_MASK = VU_SOFT_ADD_REPAIR_BAD_MASK + 4;
static constexpr int VU_SOFT_ADD_REPAIR_CONTEXT_SIZE =
	(VU_SOFT_ADD_REPAIR_ACTIVE_MASK + 4 + 15) & ~15;
static constexpr sptr VU_SOFT_RESULT_VALUE_OFFSET = offsetof(VuSoftFmacJitResult, value);



static VuUpperFmacSoftOperandSource mVUselectUpperSoftOperandSource(microVU& mVU, int opCase)
{
	if (opCase == 3)
		return VuUpperFmacSoftOperandSource::I;
	if (opCase == 4)
		return VuUpperFmacSoftOperandSource::Q;
	if (opCase != 2)
		return VuUpperFmacSoftOperandSource::Ft;

	switch (_bc_)
	{
		case 0:
			return VuUpperFmacSoftOperandSource::X;
		case 1:
			return VuUpperFmacSoftOperandSource::Y;
		case 2:
			return VuUpperFmacSoftOperandSource::Z;
		default:
			return VuUpperFmacSoftOperandSource::W;
	}
}

static VuUpperFmacSoftDescriptor mVUmakeUpperSoftDescriptor(microVU& mVU, int opCase,
	VuUpperFmacSoftKind kind, VuUpperFmacSoftDestination destination)
{
	return {kind, mVUselectUpperSoftOperandSource(mVU, opCase), destination};
}

static bool mVUupperSoftNeedsTruncateMxcsr(microVU& mVU)
{
	const FPControlRegister& fpcr = mVU.index == 0 ? EmuConfig.Cpu.VU0FPCR : EmuConfig.Cpu.VU1FPCR;
	// Soft-float dispatchers already run the complete VU block in the PS2's
	// truncate/DAZ/FTZ mode, independent of compatibility rounding overrides.
	return !CHECK_VU_SOFT(mVU.index) && fpcr.GetRoundMode() != FPRoundMode::ChopZero;
}

static bool mVUupperSoftNeedsStatusValue(microVU& mVU)
{
	return sFLAG.doFlag && sFLAG.doValue;
}

template <typename MacFlags>
static void mVUemitUpperStatusFromMacFlags(const MacFlags& mac_flags, const xIndirect32& status_flags)
{
	xMOV(eax, mac_flags);
	if (g_cpu.hasFastPext)
	{
		xMOV(ecx, eax);
		xSHR(ecx, 2);
		xOR(eax, ecx);
		xMOV(ecx, eax);
		xSHR(ecx, 1);
		xOR(eax, ecx);
		xAND(eax, 0x1111);
		xMOV(ecx, 0x1111);
		xPEXT(ecx, eax, ecx);
		xMOV(status_flags, ecx);
		return;
	}

	xMOV(ecx, eax);
	xAND(eax, 0x7777);
	xADD(eax, 0x7777);
	xOR(eax, ecx);
	xAND(eax, 0x8888);
	xMUL(ecx, eax, 0x249);
	xSHR(ecx, 12);
	xAND(ecx, 0xf);

	xMOV(status_flags, ecx);
}

static void mVUemitSoftMFlagWriteback(microVU& mVU, int result_offset)
{
	const auto resultPtr = [result_offset](int offset) {
		return ptr32[rsp + result_offset + offset];
	};

	if (mFLAG.doFlag)
	{
		xMOV(gprT1, resultPtr(offsetof(VuSoftFmacJitResult, mac_flags)));
		mVUallocMFLAGb(mVU, gprT1, mFLAG.write);
	}
}

static void mVUemitSoftMaskInactiveAccFlags(microVU& mVU, int result_offset)
{
	const auto resultPtr = [result_offset](int offset) {
		return ptr32[rsp + result_offset + offset];
	};
	const u32 lane_mask = static_cast<u32>(_X_Y_Z_W);
	const u32 mac_mask = lane_mask | (lane_mask << 4) | (lane_mask << 8) | (lane_mask << 12);

	xAND(resultPtr(offsetof(VuSoftFmacJitResult, mac_flags)), mac_mask);
	mVUemitUpperStatusFromMacFlags(
		resultPtr(offsetof(VuSoftFmacJitResult, mac_flags)),
		resultPtr(offsetof(VuSoftFmacJitResult, status_flags)));
	xOR(gprT2, resultPtr(offsetof(VuSoftFmacJitResult, mul_stage_status_flags)));
	xMOV(resultPtr(offsetof(VuSoftFmacJitResult, sticky_status_flags)), gprT2);
}

// FMAC semantics are shared; micro mode publishes a delayed flag instance.
static void mVUemitSoftSFlagWriteback(microVU& mVU, const x32& current, const x32& sticky)
{
	const x32& status = mVU.cop2 ? gprF0 : getFlagReg(sFLAG.write);
	if (!mVU.cop2)
		xMOV(status, getFlagReg(sFLAG.lastWrite));
	xAND(status, ~0x3ff00u);
	xMOV(eax, current);
	xSHL(eax, 11);
	xAND(eax, 0x1800);
	xOR(status, eax);
	xSHL(current, 14);
	xAND(current, 0x30000);
	xOR(status, current);
	xMOV(eax, sticky);
	xSHL(eax, 3);
	xAND(eax, 0x18);
	xOR(status, eax);
	xSHL(sticky, 20);
	xAND(sticky, 0xc00000);
	xOR(status, sticky);
}

static void mVUemitSoftFlagWriteback(microVU& mVU, int result_offset, VuUpperFmacSoftDescriptor op)
{
	if (op.WritesAcc() && (mFLAG.doFlag || mVUupperSoftNeedsStatusValue(mVU)))
		mVUemitSoftMaskInactiveAccFlags(mVU, result_offset);
	mVUemitSoftMFlagWriteback(mVU, result_offset);
	if (mVUupperSoftNeedsStatusValue(mVU))
	{
		const auto resultPtr = [result_offset](int offset) {
			return ptr32[rsp + result_offset + offset];
		};
		xMOV(edx, resultPtr(offsetof(VuSoftFmacJitResult, status_flags)));
		xMOV(ecx, op.IsMultiplyAdd() ?
					  resultPtr(offsetof(VuSoftFmacJitResult, sticky_status_flags)) :
					  resultPtr(offsetof(VuSoftFmacJitResult, status_flags)));
		mVUemitSoftSFlagWriteback(mVU, edx, ecx);
	}
}

static void mVUemitUpperInlineSoftAccOverflowWriteback(microVU& mVU, int result_offset)
{
	const auto resultPtr = [result_offset](int offset) {
		return ptr32[rsp + result_offset + offset];
	};

	xMOV(gprT1, ptr32[&mVU.regs().accflag]);
	xAND(gprT1, ~static_cast<u32>(_X_Y_Z_W));
	xMOV(gprT2, resultPtr(offsetof(VuSoftFmacJitResult, acc_overflow_mask)));
	xAND(gprT2, static_cast<u32>(_X_Y_Z_W));
	xOR(gprT1, gprT2);
	xMOV(ptr32[&mVU.regs().accflag], gprT1);
}


static void mVUemitUpperInlineExtractOperand(const x32& dst, const xmm& operand, VuUpperFmacSoftDescriptor op, int lane)
{
	const int variant = op.OperandVariant();
	const int operand_lane = variant >= 3 ? variant - 3 : (variant == 0 ? lane : 0);
	mVUemitExtractLane(dst, operand, operand_lane);
}

static void mVUemitUpperInlinePrepareOperand(const xmm& prepared, const xmm& operand, int variant)
{
	xMOVAPS(prepared, operand);
	if (variant != 0)
		xPSHUF.D(prepared, prepared, variant >= 3 ? (variant - 3) * 0x55 : 0);
}

static void mVUemitUpperInlineCommitResult(microVU& mVU, const xmm& destination, const xmm& result)
{
	if (_XYZW_SS)
	{
		const int active_lane = _X ? 0 : (_Y ? 1 : (_Z ? 2 : 3));
		mVUemitExtractLane(eax, result, active_lane);
		xPINSR.D(destination, eax, 0);
	}
	else if (_X_Y_Z_W == 0xf)
	{
		xMOVAPS(destination, result);
	}
	else
	{
		xBLEND.PS(destination, result, s_vu_soft_lane_mask[_X_Y_Z_W]);
	}
}

static void mVUemitUpperInlineAddSubExactLane(microVU& mVU, int result_offset, int scratch_base,
	VuUpperFmacSoftDescriptor op, const xmm& source, const xmm& operand, const xmm& destination, int lane)
{
	constexpr int lane_overflow = 1;
	constexpr int lane_underflow = 2;
	const int add_self = scratch_base;
	const int add_other = add_self + 4;
	const int add_exponent = add_other + 4;
	const int add_sign = add_exponent + 4;
	const int add_shift = add_sign + 4;
	const int lane_flags = add_shift + 4;
	const int lane_shift = 3 - lane;
	const auto resultPtr = [result_offset](int offset) {
		return ptr32[rsp + result_offset + offset];
	};
	mVUemitExtractLane(eax, source, lane);
	xMOV(ptr32[rsp + add_self], eax);
	mVUemitUpperInlineExtractOperand(edx, operand, op, lane);
	if (op.IsKind(VuUpperFmacSoftKind::Sub))
		xXOR(edx, 0x80000000);
	xMOV(ptr32[rsp + add_other], edx);
	xMOV(ptr32[rsp + lane_flags], 0);

	xMOV(ecx, eax);
	xSHR(ecx, 23);
	xAND(ecx, 0xff);
	xMOV(edx, ptr32[rsp + add_other]);
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
	xAND(ptr32[rsp + add_self], edx);
	xMOV(eax, ptr32[rsp + add_self]);
	xMOV(edx, ptr32[rsp + add_other]);
	xMOV(ptr32[rsp + add_self], edx);
	xMOV(ptr32[rsp + add_other], eax);
	xForwardJump32 add_operands_ready_from_other;

	add_truncate_other.SetTarget();
	xDEC(ecx);
	xMOV(edx, 0xffffffff);
	xSHL(edx, cl);
	xINC(ecx);
	xAND(ptr32[rsp + add_other], edx);

	add_operands_ready.SetTarget();
	add_operands_ready_from_other.SetTarget();
	xMOV(ptr32[rsp + add_shift], ecx);
	xMOV(eax, ptr32[rsp + add_self]);
	xMOV(edx, eax);
	xSHR(edx, 23);
	xAND(edx, 0xff);
	xMOV(ptr32[rsp + add_exponent], edx);
	xMOV(edx, eax);
	xSAR(edx, 31);
	xAND(eax, 0x7fffff);
	xOR(eax, 0x800000);
	xXOR(eax, edx);
	xSUB(eax, edx);
	xSHL(eax, 6);
	xMOV(ptr32[rsp + add_self], eax);

	xMOV(eax, ptr32[rsp + add_other]);
	xMOV(edx, eax);
	xSAR(edx, 31);
	xAND(eax, 0x7fffff);
	xOR(eax, 0x800000);
	xXOR(eax, edx);
	xSUB(eax, edx);
	xSHL(eax, 6);
	xMOV(ecx, ptr32[rsp + add_shift]);
	xSAR(eax, cl);
	xADD(eax, ptr32[rsp + add_self]);
	xMOV(edx, eax);
	xAND(edx, 0x80000000);
	xMOV(ptr32[rsp + add_sign], edx);
	xMOV(edx, eax);
	xSAR(edx, 31);
	xXOR(eax, edx);
	xSUB(eax, edx);
	xForwardJZ32 add_result_zero;

	xBSR(ecx, eax);
	xMOV(edx, ptr32[rsp + add_exponent]);
	xADD(edx, ecx);
	xSUB(edx, 29);
	xMOV(ptr32[rsp + add_exponent], edx);
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
	xMOV(edx, ptr32[rsp + add_exponent]);
	xCMP(edx, 255);
	xForwardJG32 add_overflow_result;
	xCMP(edx, 1);
	xForwardJL32 add_underflow_result;
	xSHL(edx, 23);
	xOR(eax, edx);
	xOR(eax, ptr32[rsp + add_sign]);
	xForwardJump32 add_result_ready;

	add_overflow_result.SetTarget();
	xMOV(eax, ptr32[rsp + add_sign]);
	xOR(eax, 0x7fffffff);
	xMOV(ptr32[rsp + lane_flags], lane_overflow);
	xForwardJump32 add_result_ready_from_overflow;

	add_underflow_result.SetTarget();
	xOR(eax, ptr32[rsp + add_sign]);
	xMOV(ptr32[rsp + lane_flags], lane_underflow);
	xForwardJump32 add_result_ready_from_underflow;

	add_result_zero.SetTarget();
	xXOR(eax, eax);
	xForwardJump32 add_result_ready_from_zero;

	add_self_denormal.SetTarget();
	xTEST(edx, edx);
	xForwardJZ32 add_both_denormal;
	xMOV(eax, ptr32[rsp + add_other]);
	xForwardJump32 add_result_ready_from_self_denormal;

	add_both_denormal.SetTarget();
	xMOV(eax, ptr32[rsp + add_self]);
	xAND(eax, 0x80000000);
	xMOV(edx, ptr32[rsp + add_other]);
	xAND(eax, edx);
	xForwardJump32 add_result_ready_from_both_denormal;

	add_other_denormal.SetTarget();
	add_result_self_large_diff.SetTarget();
	xMOV(eax, ptr32[rsp + add_self]);
	xForwardJump32 add_result_ready_from_self;

	add_result_other_large_diff.SetTarget();
	xMOV(eax, ptr32[rsp + add_other]);

	add_result_ready.SetTarget();
	add_result_ready_from_overflow.SetTarget();
	add_result_ready_from_underflow.SetTarget();
	add_result_ready_from_zero.SetTarget();
	add_result_ready_from_self_denormal.SetTarget();
	add_result_ready_from_both_denormal.SetTarget();
	add_result_ready_from_self.SetTarget();
	xPINSR.D(destination, eax, _XYZW_SS ? 0 : lane);

	xTEST(eax, 0x80000000);
	xForwardJZ8 result_positive;
	xOR(resultPtr(offsetof(VuSoftFmacJitResult, mac_flags)), 0x0010u << lane_shift);
	xOR(resultPtr(offsetof(VuSoftFmacJitResult, status_flags)), 0x2);
	result_positive.SetTarget();
	xTEST(ptr32[rsp + lane_flags], lane_underflow);
	xForwardJZ8 result_not_underflow;
	xOR(resultPtr(offsetof(VuSoftFmacJitResult, mac_flags)), 0x0101u << lane_shift);
	xOR(resultPtr(offsetof(VuSoftFmacJitResult, status_flags)), 0x5);
	xForwardJump32 result_flags_ready;
	result_not_underflow.SetTarget();
	xMOV(edx, eax);
	xAND(edx, 0x7fffffff);
	xForwardJNZ8 result_not_zero;
	xOR(resultPtr(offsetof(VuSoftFmacJitResult, mac_flags)), 0x0001u << lane_shift);
	xOR(resultPtr(offsetof(VuSoftFmacJitResult, status_flags)), 0x1);
	xForwardJump32 result_flags_ready_from_zero;
	result_not_zero.SetTarget();
	xTEST(ptr32[rsp + lane_flags], lane_overflow);
	xForwardJZ8 result_flags_ready_nonexception;
	xOR(resultPtr(offsetof(VuSoftFmacJitResult, mac_flags)), 0x1000u << lane_shift);
	xOR(resultPtr(offsetof(VuSoftFmacJitResult, status_flags)), 0x8);
	result_flags_ready.SetTarget();
	result_flags_ready_from_zero.SetTarget();
	result_flags_ready_nonexception.SetTarget();

	if (op.WritesAcc())
	{
		const u32 lane_bit = 1u << lane_shift;
		xTEST(ptr32[rsp + lane_flags], lane_overflow);
		xForwardJZ8 result_acc_not_overflow;
		xOR(resultPtr(offsetof(VuSoftFmacJitResult, acc_overflow_mask)), lane_bit);
		result_acc_not_overflow.SetTarget();
	}
}

static void mVUemitUpperInlineAddSubExactResult(microVU& mVU, VuUpperFmacSoftDescriptor op)
{
	{
		// The inline path only clobbers allocator-visible EDX. Keep the other
		// caller-saved VI mappings live across the common packed path.
		mVU.regAlloc->writeBackReg(edx, true);
		mVU.regAlloc->clearGPR(edx);
	}
	constexpr sptr result_offset = 0;
	const auto resultPtr = [](int offset) {
		return ptr32[rsp + result_offset + offset];
	};
	const int variant = op.OperandVariant();
	const xmm& source = mVU.regAlloc->allocReg(_Fs_);
	xmm operand;
	if (variant == 1)
	{
		operand = mVU.regAlloc->allocReg(33);
	}
	else if (variant == 2)
	{
		operand = mVU.regAlloc->allocReg();
		getQreg(operand, mVUinfo.readQ);
	}
	else
	{
		operand = mVU.regAlloc->allocReg(_Ft_);
	}
	const int destination_reg = op.WritesAcc() ? 32 : _Fd_;
	const int destination_load = _X_Y_Z_W == 0xf ? -1 : destination_reg;
	const xmm& destination = mVU.regAlloc->allocReg(destination_load, destination_reg, _X_Y_Z_W);
	const bool can_use_vector_native =
		g_cpu.vectorISA >= ProcessorFeatures::VectorISA::AVX2 && _X_Y_Z_W != 0;
	const bool use_packed_native = can_use_vector_native && !_XYZW_SS;
	const bool switch_mxcsr = mVUupperSoftNeedsTruncateMxcsr(mVU);
	const FPControlRegister& vu_fpcr =
		mVU.index == 0 ? EmuConfig.Cpu.VU0FPCR : EmuConfig.Cpu.VU1FPCR;
	const bool native_ps2_zero =
		vu_fpcr.GetFlushToZero() && vu_fpcr.GetDenormalsAreZero();
	const bool use_stackless_scalar_flags = can_use_vector_native && !switch_mxcsr &&
	                                        _XYZW_SS && (mFLAG.doFlag || sFLAG.doFlag);
	const bool use_stackless_add = can_use_vector_native && !switch_mxcsr &&
	                               ((!mFLAG.doFlag && !sFLAG.doFlag) || use_stackless_scalar_flags);
	const bool use_vector_temporaries = use_packed_native || use_stackless_add;
	const int scratch_base = use_packed_native ? VU_SOFT_ADD_REPAIR_CONTEXT_SIZE :
	                                             ((sizeof(VuSoftFmacJitResult) + 15) & ~15);
	const int packed_destination = scratch_base;
	const int packed_shift = packed_destination + 16;
	const int scratch_end = packed_shift + 16;
	const int stack_size = (scratch_end + 15) & ~15;
	xmm native_source, native_operand, work, shift_mask, native_guard;
	if (use_vector_temporaries)
	{
		native_source = mVU.regAlloc->allocReg();
		native_operand = mVU.regAlloc->allocReg();
		work = mVU.regAlloc->allocReg();
		shift_mask = mVU.regAlloc->allocReg();
		native_guard = _X_Y_Z_W == 0xf ? destination : mVU.regAlloc->allocReg();
	}
	std::optional<xForwardJump32> stackless_add_finished;
	std::optional<xForwardJump32> stackless_add_zero_input_failed;
	std::optional<xForwardJump32> stackless_add_input_failed;
	std::optional<xForwardJump32> stackless_add_result_failed;
	if (use_stackless_add)
	{
		xMOVAPS(native_source, source);
		xMOVAPS(native_operand, operand);
		const int scalar_lane = _X ? 0 : (_Y ? 1 : (_Z ? 2 : 3));
		if (_XYZW_SS)
			xPSHUF.D(native_source, native_source, scalar_lane * 0x55);
		if (variant != 0)
			xPSHUF.D(native_operand, native_operand, variant >= 3 ? (variant - 3) * 0x55 : 0);
		else if (_XYZW_SS)
			xPSHUF.D(native_operand, native_operand, scalar_lane * 0x55);
		if (op.IsKind(VuUpperFmacSoftKind::Sub))
			xPXOR(native_operand, ptr128[s_vu_soft_sign]);
		if (_X_Y_Z_W != 0xf && !_XYZW_SS)
		{
			const u8 native_active_mask = s_vu_soft_lane_mask[_X_Y_Z_W];
			const u8 inactive_mask = (~native_active_mask) & 0xf;
			xBLEND.PS(native_source, ptr128[s_vu_soft_float_one], inactive_mask);
			xBLEND.PS(native_operand, ptr128[s_vu_soft_float_one], inactive_mask);
		}

		// DAZ already canonicalizes PS2-zero inputs for host arithmetic. Raw
		// exponent extraction remains necessary for the PS2 alignment rule.
		xPAND(work, native_source, ptr128[s_vu_soft_exp_field]);
		xPAND(shift_mask, native_operand, ptr128[s_vu_soft_exp_field]);
		if (native_ps2_zero)
		{
			if (use_stackless_scalar_flags)
			{
				// Flag-producing scalar operations need to distinguish exact
				// cancellation from a flushed underflow. Keep PS2-zero operands
				// on the complete integer path where their sign rules are explicit.
				xPMIN.UD(native_guard, work, shift_mask);
				xPTEST(native_guard, native_guard);
				stackless_add_zero_input_failed.emplace(Jcc_Zero);
			}
			// Restrict the common path to operands whose sum cannot reach exponent
			// 255. This also excludes extended inputs and makes the post-add upper
			// boundary check unnecessary.
			xPMAX.UD(native_guard, work, shift_mask);
			xPCMP.GTD(native_guard, ptr128[s_vu_soft_exp_field_253]);
			xPTEST(native_guard, native_guard);
			stackless_add_input_failed.emplace(Jcc_NotZero);
		}
		else
		{
			xPCMP.EQD(native_guard, work, ptr128[s_vu_soft_exp_field]);
			xMOVMSKPS(eax, native_guard);
			xPCMP.EQD(native_guard, work, ptr128[s_vu_soft_zero]);
			xPAND(native_guard, ptr128[s_vu_soft_abs]);
			xPXOR(native_guard, ptr128[s_vu_soft_all_ones]);
			xPAND(native_source, native_guard);

			xPCMP.EQD(native_guard, shift_mask, ptr128[s_vu_soft_exp_field]);
			xMOVMSKPS(edx, native_guard);
			xOR(eax, edx);
			xPCMP.EQD(native_guard, shift_mask, ptr128[s_vu_soft_zero]);
			xPAND(native_guard, ptr128[s_vu_soft_abs]);
			xPXOR(native_guard, ptr128[s_vu_soft_all_ones]);
			xPAND(native_operand, native_guard);
			xTEST(eax, eax);
			stackless_add_input_failed.emplace(Jcc_NotZero);
		}

		// Pretruncate the smaller operand exactly as the packed flag-producing path.
		xPSUB.D(work, shift_mask);
		xPSRA.D(work, 23);
		xPABS.D(shift_mask, work);
		xPCMP.GTD(native_guard, shift_mask, ptr128[s_vu_soft_exp_24]);
		xPBLEND.VB(shift_mask, shift_mask, ptr128[s_vu_soft_exp_33], native_guard);
		xPSUB.D(shift_mask, ptr128[s_vu_soft_one]);
		xPMAX.SD(shift_mask, ptr128[s_vu_soft_zero]);
		xPCMP.GTD(native_guard, work, ptr128[s_vu_soft_zero]);
		xPBLEND.VB(work, native_operand, native_source, native_guard);
		xPBLEND.VB(native_operand, native_source, native_operand, native_guard);
		xVPSRLVD(native_operand, native_operand, shift_mask);
		xVPSLLVD(native_operand, native_operand, shift_mask);
		xMOVAPS(native_source, work);
		if (use_stackless_scalar_flags)
		{
			xMOVAPS(shift_mask, native_operand);
			xPXOR(shift_mask, ptr128[s_vu_soft_sign]);
			xPCMP.EQD(shift_mask, native_source);
		}

		if (!native_ps2_zero)
		{
			// A host zero is valid only for exact cancellation or two PS2-zero terms.
			xPXOR(work, native_operand, ptr128[s_vu_soft_sign]);
			xPCMP.EQD(work, native_source);
			xPAND(native_guard, native_source, ptr128[s_vu_soft_abs]);
			xPCMP.EQD(native_guard, ptr128[s_vu_soft_zero]);
			xPAND(shift_mask, native_operand, ptr128[s_vu_soft_abs]);
			xPCMP.EQD(shift_mask, ptr128[s_vu_soft_zero]);
			xPAND(native_guard, shift_mask);
			xPOR(work, native_guard);
		}
		xADD.PS(native_source, native_operand);

		if (!native_ps2_zero)
		{
			xPAND(native_operand, native_source, ptr128[s_vu_soft_abs]);
			xPCMP.GTD(shift_mask, native_operand, ptr128[s_vu_soft_max_safe]);
			xMOVAPS(native_guard, ptr128[s_vu_soft_hidden_bit]);
			xPCMP.GTD(native_guard, native_operand);
			xPANDN(work, native_guard);
			xPOR(shift_mask, work);
			xPTEST(shift_mask, shift_mask);
			stackless_add_result_failed.emplace(Jcc_NotZero);
		}

		if (_XYZW_SS)
		{
			xMOVD(eax, native_source);
			xPINSR.D(destination, eax, 0);
		}
		else if (_X_Y_Z_W == 0xf)
			xMOVAPS(destination, native_source);
		else
			xBLEND.PS(destination, native_source, s_vu_soft_lane_mask[_X_Y_Z_W]);
		if (op.WritesAcc())
			xAND(ptr32[&mVU.regs().accflag], ~static_cast<u32>(_X_Y_Z_W));
		if (use_stackless_scalar_flags)
		{
			constexpr sptr scalar_result_offset = 0;
			constexpr int scalar_stack_size = (sizeof(VuSoftFmacJitResult) + 15) & ~15;
			const auto scalarResultPtr = [](int offset) {
				return ptr32[rsp + scalar_result_offset + offset];
			};
			const int lane_shift = 3 - scalar_lane;
			mVUemitUpperSoftStackAlloc(scalar_stack_size);
			xMOV(scalarResultPtr(offsetof(VuSoftFmacJitResult, mac_flags)), 0);
			xMOV(scalarResultPtr(offsetof(VuSoftFmacJitResult, status_flags)), 0);
			xMOV(scalarResultPtr(offsetof(VuSoftFmacJitResult, acc_overflow_mask)), 0);
			xMOVD(eax, native_source);
			xTEST(eax, 0x80000000);
			xForwardJZ8 scalar_result_positive;
			xOR(scalarResultPtr(offsetof(VuSoftFmacJitResult, mac_flags)), 0x0010u << lane_shift);
			xOR(scalarResultPtr(offsetof(VuSoftFmacJitResult, status_flags)), 0x2);
			scalar_result_positive.SetTarget();
			xMOV(edx, eax);
			xAND(edx, 0x7fffffff);
			xForwardJNZ8 scalar_result_flags_ready;
			xMOVMSKPS(edx, shift_mask);
			xTEST(edx, 1);
			xForwardJZ8 scalar_result_underflow;
			xOR(scalarResultPtr(offsetof(VuSoftFmacJitResult, mac_flags)), 0x0001u << lane_shift);
			xOR(scalarResultPtr(offsetof(VuSoftFmacJitResult, status_flags)), 0x1);
			xForwardJump8 scalar_result_flags_ready_from_zero;
			scalar_result_underflow.SetTarget();
			xOR(scalarResultPtr(offsetof(VuSoftFmacJitResult, mac_flags)), 0x0101u << lane_shift);
			xOR(scalarResultPtr(offsetof(VuSoftFmacJitResult, status_flags)), 0x5);
			scalar_result_flags_ready.SetTarget();
			scalar_result_flags_ready_from_zero.SetTarget();
			if (op.WritesAcc())
				mVUemitUpperInlineSoftAccOverflowWriteback(mVU, scalar_result_offset);
			mVUemitSoftFlagWriteback(mVU, scalar_result_offset, op);
			mVUemitUpperSoftStackFree(scalar_stack_size);
		}
		stackless_add_finished.emplace();
		if (stackless_add_zero_input_failed.has_value())
			stackless_add_zero_input_failed->SetTarget();
		if (stackless_add_input_failed.has_value())
			stackless_add_input_failed->SetTarget();
		if (stackless_add_result_failed.has_value())
			stackless_add_result_failed->SetTarget();
	}

	mVUemitUpperSoftStackAlloc(stack_size);
	xMOV(resultPtr(offsetof(VuSoftFmacJitResult, mac_flags)), 0);
	xMOV(resultPtr(offsetof(VuSoftFmacJitResult, status_flags)), 0);
	xMOV(resultPtr(offsetof(VuSoftFmacJitResult, acc_overflow_mask)), 0);

	const auto emit_exact = [&]() {
		if (_X)
			mVUemitUpperInlineAddSubExactLane(mVU, result_offset, scratch_base, op, source, operand, destination, 0);
		if (_Y)
			mVUemitUpperInlineAddSubExactLane(mVU, result_offset, scratch_base, op, source, operand, destination, 1);
		if (_Z)
			mVUemitUpperInlineAddSubExactLane(mVU, result_offset, scratch_base, op, source, operand, destination, 2);
		if (_W)
			mVUemitUpperInlineAddSubExactLane(mVU, result_offset, scratch_base, op, source, operand, destination, 3);
	};

	if (use_packed_native)
	{
		if (_X_Y_Z_W != 0xf)
			xMOVAPS(ptr128[rsp + packed_destination], destination);
		xMOVAPS(native_source, source);
		xMOVAPS(native_operand, operand);
		if (variant != 0)
			xPSHUF.D(native_operand, native_operand, variant >= 3 ? (variant - 3) * 0x55 : 0);
		if (op.IsKind(VuUpperFmacSoftKind::Sub))
			xPXOR(native_operand, ptr128[s_vu_soft_sign]);
		if (_X_Y_Z_W != 0xf)
		{
			const u8 inactive_mask = (~s_vu_soft_lane_mask[_X_Y_Z_W]) & 0xf;
			xBLEND.PS(native_source, ptr128[s_vu_soft_float_one], inactive_mask);
			xBLEND.PS(native_operand, ptr128[s_vu_soft_float_one], inactive_mask);
		}

		xPSRL.D(work, native_source, 23);
		xPAND(work, ptr128[s_vu_soft_exp_mask]);
		xPCMP.EQD(native_guard, work, ptr128[s_vu_soft_exp_255]);
		xMOVMSKPS(eax, native_guard);
		xPCMP.EQD(native_guard, work, ptr128[s_vu_soft_zero]);
		xPAND(native_guard, ptr128[s_vu_soft_abs]);
		xPXOR(native_guard, ptr128[s_vu_soft_all_ones]);
		xPAND(native_source, native_guard);

		xPSRL.D(shift_mask, native_operand, 23);
		xPAND(shift_mask, ptr128[s_vu_soft_exp_mask]);
		xPCMP.EQD(native_guard, shift_mask, ptr128[s_vu_soft_exp_255]);
		xMOVMSKPS(edx, native_guard);
		xOR(eax, edx);
		xPCMP.EQD(native_guard, shift_mask, ptr128[s_vu_soft_zero]);
		xPAND(native_guard, ptr128[s_vu_soft_abs]);
		xPXOR(native_guard, ptr128[s_vu_soft_all_ones]);
		xPAND(native_operand, native_guard);

		xPSUB.D(work, shift_mask);
		xPABS.D(shift_mask, work);
		xPCMP.GTD(native_guard, shift_mask, ptr128[s_vu_soft_exp_24]);
		xPBLEND.VB(shift_mask, shift_mask, ptr128[s_vu_soft_exp_33], native_guard);
		xAND(eax, s_vu_soft_lane_mask[_X_Y_Z_W]);
		xForwardJNZ32 use_repair_from_input;

		xPSUB.D(shift_mask, ptr128[s_vu_soft_one]);
		xPMAX.SD(shift_mask, ptr128[s_vu_soft_zero]);
		xPCMP.GTD(native_guard, work, ptr128[s_vu_soft_zero]);
		xPBLEND.VB(work, native_operand, native_source, native_guard);
		xPBLEND.VB(native_operand, native_source, native_operand, native_guard);
		xVPSRLVD(native_operand, native_operand, shift_mask);
		xVPSLLVD(native_operand, native_operand, shift_mask);
		xMOVAPS(native_source, work);

		// FTZ can turn a true PS2 underflow into host zero. Only exact
		// cancellation and two PS2-zero operands may accept a zero result.
		xPXOR(native_guard, native_operand, ptr128[s_vu_soft_sign]);
		xPCMP.EQD(native_guard, native_source);
		xPAND(work, native_source, ptr128[s_vu_soft_abs]);
		xPCMP.EQD(work, ptr128[s_vu_soft_zero]);
		xPAND(shift_mask, native_operand, ptr128[s_vu_soft_abs]);
		xPCMP.EQD(shift_mask, ptr128[s_vu_soft_zero]);
		xPAND(work, shift_mask);
		xPOR(native_guard, work);
		xMOVAPS(ptr128[rsp + packed_shift], native_guard);

		if (switch_mxcsr)
			xLDMXCSR(ptr32[&s_vu_soft_truncate_mxcsr]);
		xADD.PS(native_source, native_operand);
		if (switch_mxcsr)
			xLDMXCSR(ptr32[mVU.index == 0 ? &EmuConfig.Cpu.VU0FPCR.bitmask : &EmuConfig.Cpu.VU1FPCR.bitmask]);

		xPAND(native_operand, native_source, ptr128[s_vu_soft_abs]);
		xPCMP.GTD(shift_mask, native_operand, ptr128[s_vu_soft_max_safe]);
		xMOVAPS(native_guard, ptr128[rsp + packed_shift]);
		xMOVAPS(work, ptr128[s_vu_soft_hidden_bit]);
		xPCMP.GTD(work, native_operand);
		xPANDN(native_guard, work);
		xPOR(shift_mask, native_guard);
		xMOVMSKPS(eax, shift_mask);
		xAND(eax, s_vu_soft_lane_mask[_X_Y_Z_W]);
		xForwardJNZ32 use_repair_from_result;

		if (_X_Y_Z_W == 0xf)
		{
			xMOVAPS(destination, native_source);
		}
		else
		{
			xMOVAPS(destination, ptr128[rsp + packed_destination]);
			xBLEND.PS(destination, native_source, s_vu_soft_lane_mask[_X_Y_Z_W]);
		}
		xMOVMSKPS(eax, native_source);
		xAND(eax, s_vu_soft_lane_mask[_X_Y_Z_W]);
		xMOV64(rcx, reinterpret_cast<uptr>(s_vu_soft_lane_mask));
		xMOVZX(eax, ptr8[xAddressVoid(rcx, rax, 1)]);
		xSHL(eax, 4);
		xPAND(work, native_source, ptr128[s_vu_soft_abs]);
		xPCMP.EQD(work, ptr128[s_vu_soft_zero]);
		xMOVMSKPS(edx, work);
		xAND(edx, s_vu_soft_lane_mask[_X_Y_Z_W]);
		xMOVZX(edx, ptr8[xAddressVoid(rcx, rdx, 1)]);
		xOR(eax, edx);
		xMOV(resultPtr(offsetof(VuSoftFmacJitResult, mac_flags)), eax);
		xXOR(edx, edx);
		xTEST(eax, 0xf);
		xSETNZ(dl);
		xTEST(eax, 0xf0);
		xSETNZ(al);
		xMOVZX(eax, al);
		xSHL(eax, 1);
		xOR(edx, eax);
		xMOV(resultPtr(offsetof(VuSoftFmacJitResult, status_flags)), edx);
		xForwardJump32 arithmetic_ready;
		use_repair_from_input.SetTarget();
		use_repair_from_result.SetTarget();
		// Both guarded edges leave an x86-lane bad mask in EAX. Convert it to
		// the architectural XYZW bit order consumed by the shared repair helper.
		xMOV64(rdx, reinterpret_cast<uptr>(s_vu_soft_lane_mask));
		xMOVZX(eax, ptr8[xAddressVoid(rdx, rax, 1)]);
		xAND(eax, static_cast<u32>(_X_Y_Z_W));
		xMOV(resultPtr(VU_SOFT_ADD_REPAIR_BAD_MASK), eax);
		xMOV(resultPtr(VU_SOFT_ADD_REPAIR_ACTIVE_MASK), static_cast<u32>(_X_Y_Z_W));
		xMOVAPS(ptr128[rsp + VU_SOFT_ADD_REPAIR_SOURCE], source);
		xMOVAPS(work, operand);
		if (variant != 0)
			xPSHUF.D(work, work, variant >= 3 ? (variant - 3) * 0x55 : 0);
		if (op.IsKind(VuUpperFmacSoftKind::Sub))
			xPXOR(work, ptr128[s_vu_soft_sign]);
		xMOVAPS(ptr128[rsp + VU_SOFT_ADD_REPAIR_OPERAND], work);
		xLEA(rdx, ptr[rsp + result_offset]);
		xCALL(mVU.softAddLaneRepair);
		xMOVAPS(native_source, ptr128[rsp + VU_SOFT_RESULT_VALUE_OFFSET]);
		if (_X_Y_Z_W == 0xf)
			xMOVAPS(destination, native_source);
		else
		{
			xMOVAPS(destination, ptr128[rsp + packed_destination]);
			xBLEND.PS(destination, native_source, s_vu_soft_lane_mask[_X_Y_Z_W]);
		}
		arithmetic_ready.SetTarget();
	}
	else
	{
		emit_exact();
	}
	if (op.WritesAcc())
	{
		mVUemitUpperInlineSoftAccOverflowWriteback(mVU, result_offset);
	}
	mVUemitSoftFlagWriteback(mVU, result_offset, op);
	mVUemitUpperSoftStackFree(stack_size);
	if (stackless_add_finished.has_value())
		stackless_add_finished->SetTarget();
	mVU.regAlloc->clearNeeded(destination);
	if (use_vector_temporaries)
	{
		if (native_guard.Id != destination.Id)
			mVU.regAlloc->clearNeeded(native_guard);
		mVU.regAlloc->clearNeeded(shift_mask);
		mVU.regAlloc->clearNeeded(work);
		mVU.regAlloc->clearNeeded(native_operand);
		mVU.regAlloc->clearNeeded(native_source);
	}
	mVU.regAlloc->clearNeeded(operand);
	mVU.regAlloc->clearNeeded(source);
}

static void mVUGenerateSoftMulExactKernel(microVU& mVU)
{
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
	constexpr int stack_size = (add3_values + 12 * 4 + 15) & ~15;
	constexpr int product_underflow = 1;
	constexpr int product_overflow = 2;
	mVU.softMulExact = xGetAlignedCallTarget();
	xSUB(rsp, stack_size);
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
	xADD(rsp, stack_size);
	xRET();
}

static void mVUGenerateSoftMaddIntegratedLaneKernel(microVU& mVU)
{
	// Internal ABI: eax = source, edx = operand, ecx = accumulator,
	// r8d = incoming ACC overflow, r9d = subtract. Returns eax = final raw,
	// edx = result-stage flags (overflow bit 0, underflow bit 1), and ecx =
	// product-stage status nibble. The multiply is fully truncated before the
	// add begins; this is deliberately not a fused host operation.
	constexpr sptr source_raw = 0;
	constexpr int operand_raw = source_raw + 4;
	constexpr int accumulator_raw = operand_raw + 4;
	constexpr int incoming_acc_overflow = accumulator_raw + 4;
	constexpr int subtract_flag = incoming_acc_overflow + 4;
	constexpr int product_sign = subtract_flag + 4;
	constexpr int product_exponent = product_sign + 4;
	constexpr int product_significand = product_exponent + 4;
	constexpr int product_status = product_significand + 4;
	constexpr int mantissa_a = product_status + 4;
	constexpr int full_lo = mantissa_a + 4;
	constexpr int full_hi = full_lo + 4;
	constexpr int booth_negate = full_hi + 4;
	constexpr int booth_data = booth_negate + 8 * 4;
	constexpr int add3_values = booth_data + 8 * 4;
	constexpr int stack_size = (add3_values + 12 * 4 + 15) & ~15;
	constexpr int product_status_zero = 1;
	constexpr int product_status_sign = 2;
	constexpr int product_status_underflow = 4;
	constexpr int product_status_overflow = 8;
	constexpr int lane_overflow = 1;
	constexpr int lane_underflow = 2;

	mVU.softMaddIntegratedLane = xGetAlignedCallTarget();
	xSUB(rsp, stack_size);
	xMOV(ptr32[rsp + source_raw], eax);
	xMOV(ptr32[rsp + operand_raw], edx);
	xMOV(ptr32[rsp + accumulator_raw], ecx);
	xMOV(ptr32[rsp + incoming_acc_overflow], r8d);
	xMOV(ptr32[rsp + subtract_flag], r9d);
	xXOR(eax, edx);
	xAND(eax, 0x80000000);
	xMOV(ptr32[rsp + product_sign], eax);
	xSHR(eax, 30);
	xAND(eax, product_status_sign);
	xMOV(ptr32[rsp + product_status], eax);
	xMOV(ptr32[rsp + product_exponent], 0);
	xMOV(ptr32[rsp + product_significand], 0);

	xMOV(eax, ptr32[rsp + source_raw]);
	xSHR(eax, 23);
	xAND(eax, 0xff);
	xForwardJZ32 product_zero_from_source;
	xMOV(edx, ptr32[rsp + operand_raw]);
	xSHR(edx, 23);
	xAND(edx, 0xff);
	xForwardJZ32 product_zero_from_operand;
	xADD(eax, edx);
	xSUB(eax, 127);
	xMOV(ptr32[rsp + product_exponent], eax);
	xCMP(eax, 0);
	xForwardJL32 product_underflow_from_exponent;
	xCMP(eax, 255);
	xForwardJG32 product_overflow_from_exponent;
	xMOV(edx, ptr32[rsp + operand_raw]);
	xTEST(edx, 0x7fffff);
	xForwardJNZ8 product_requires_booth;
	xCMP(eax, 1);
	xForwardJL32 product_underflow_power_operand;
	xMOV(eax, ptr32[rsp + source_raw]);
	xAND(eax, 0x7fffff);
	xOR(eax, 0x800000);
	xMOV(ptr32[rsp + product_significand], eax);
	xForwardJump32 product_normal_from_power_operand;

	product_requires_booth.SetTarget();
	xMOV(eax, ptr32[rsp + source_raw]);
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

	X86SoftFloatEmitter::EmitCarrySaveAdd(booth_data + 1 * 4, booth_data + 2 * 4,
		booth_data + 3 * 4, t0_lo, t0_hi);
	xAND(ptr32[rsp + booth_data + 4 * 4], ~0x7ffu);
	xMOV(eax, ptr32[rsp + booth_data + 5 * 4]);
	xMOV(ptr32[rsp + mantissa_a], eax);
	xAND(ptr32[rsp + booth_data + 5 * 4], ~0xfffu);
	X86SoftFloatEmitter::EmitCarrySaveAdd(booth_data + 4 * 4, booth_data + 5 * 4,
		booth_data + 6 * 4, t1_lo, t1_hi);
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
	xForwardJLE8 product_significand_normalized;
	xSHR(eax, 1);
	xINC(ptr32[rsp + product_exponent]);
	product_significand_normalized.SetTarget();
	xMOV(edx, ptr32[rsp + product_exponent]);
	xCMP(edx, 255);
	xForwardJG32 product_overflow_after_normalize;
	xCMP(edx, 1);
	xForwardJL32 product_underflow_after_normalize;
	xMOV(ptr32[rsp + product_significand], eax);
	xForwardJump32 product_normal;

	product_zero_from_source.SetTarget();
	product_zero_from_operand.SetTarget();
	xOR(ptr32[rsp + product_status], product_status_zero);
	xForwardJump32 product_zero;
	product_underflow_power_operand.SetTarget();
	product_underflow_from_exponent.SetTarget();
	product_underflow_after_normalize.SetTarget();
	xOR(ptr32[rsp + product_status], product_status_zero | product_status_underflow);
	xMOV(ptr32[rsp + product_exponent], 0);
	xMOV(ptr32[rsp + product_significand], 0);
	xForwardJump32 product_underflow;
	product_overflow_from_exponent.SetTarget();
	product_overflow_after_normalize.SetTarget();
	xOR(ptr32[rsp + product_status], product_status_overflow);
	xForwardJump32 product_overflow;

	product_normal.SetTarget();
	product_normal_from_power_operand.SetTarget();
	product_zero.SetTarget();
	product_underflow.SetTarget();
	product_overflow.SetTarget();

	// Apply MSUB's sign inversion only after product-stage status is fixed.
	xMOV(r10d, ptr32[rsp + subtract_flag]);
	xSHL(r10d, 31);
	xXOR(r10d, ptr32[rsp + product_sign]);
	xMOV(ptr32[rsp + product_sign], r10d);
	xTEST(ptr32[rsp + incoming_acc_overflow], 0xffffffff);
	xForwardJZ32 accumulator_not_overflow;
	xTEST(ptr32[rsp + product_status], product_status_overflow);
	xForwardJNZ32 mac_exception_limit;
	xMOV(eax, ptr32[rsp + accumulator_raw]);
	xMOV(edx, lane_overflow);
	xForwardJump32 add_result_ready_from_acc_overflow;

	accumulator_not_overflow.SetTarget();
	xTEST(ptr32[rsp + product_status], product_status_overflow);
	xForwardJZ32 regular_add;
	mac_exception_limit.SetTarget();
	xMOV(eax, 0x7fffffff);
	xTEST(r10d, 0x80000000);
	xForwardJZ8 mac_exception_value_ready;
	xMOV(eax, 0xffffffff);
	mac_exception_value_ready.SetTarget();
	xMOV(edx, lane_overflow);
	xForwardJump32 add_result_ready_from_mac_exception;

	regular_add.SetTarget();
	xMOV(eax, ptr32[rsp + accumulator_raw]);
	xMOV(ecx, eax);
	xSHR(ecx, 23);
	xAND(ecx, 0xff);
	xMOV(edx, ptr32[rsp + product_exponent]);
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
	xForwardJL32 add_product_is_larger;

	// ACC is the larger (or equal-exponent) term.
	xMOV(r9d, ptr32[rsp + accumulator_raw]);
	xSHR(r9d, 23);
	xAND(r9d, 0xff);
	xMOV(eax, ptr32[rsp + accumulator_raw]);
	xMOV(edx, eax);
	xAND(eax, 0x7fffff);
	xOR(eax, 0x800000);
	xMOV(r10d, ptr32[rsp + product_significand]);
	xMOV(r11d, ptr32[rsp + product_sign]);
	xForwardJump32 add_magnitudes_selected;

	add_product_is_larger.SetTarget();
	xNEG(ecx);
	xMOV(r9d, ptr32[rsp + product_exponent]);
	xMOV(eax, ptr32[rsp + product_significand]);
	xMOV(edx, ptr32[rsp + product_sign]);
	xMOV(r10d, ptr32[rsp + accumulator_raw]);
	xMOV(r11d, r10d);
	xAND(r10d, 0x7fffff);
	xOR(r10d, 0x800000);
	add_magnitudes_selected.SetTarget();

	// Clear the smaller term's low (gap - 1) bits before alignment, exactly
	// matching PS2Float::Add/Sub. A one-bit exponent gap clears no bits.
	xTEST(ecx, ecx);
	xForwardJZ8 add_smaller_truncated;
	xMOV(r8d, 0xffffffff);
	xDEC(ecx);
	xSHL(r8d, cl);
	xINC(ecx);
	xAND(r10d, r8d);
	add_smaller_truncated.SetTarget();
	xSAR(r11d, 31);
	xXOR(r10d, r11d);
	xSUB(r10d, r11d);
	xSHL(r10d, 6);
	xSAR(r10d, cl);
	xSAR(edx, 31);
	xXOR(eax, edx);
	xSUB(eax, edx);
	xSHL(eax, 6);
	xADD(eax, r10d);
	xMOV(r8d, r9d);
	xMOV(edx, eax);
	xAND(edx, 0x80000000);
	xMOV(r9d, edx);
	xMOV(edx, eax);
	xSAR(edx, 31);
	xXOR(eax, edx);
	xSUB(eax, edx);
	xForwardJZ32 add_result_zero;

	xBSR(ecx, eax);
	xADD(r8d, ecx);
	xSUB(r8d, 29);
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
	xCMP(r8d, 255);
	xForwardJG32 add_overflow_result;
	xCMP(r8d, 1);
	xForwardJL32 add_underflow_result;
	xSHL(r8d, 23);
	xOR(eax, r8d);
	xOR(eax, r9d);
	xXOR(edx, edx);
	xForwardJump32 add_result_ready;

	add_overflow_result.SetTarget();
	xMOV(eax, r9d);
	xOR(eax, 0x7fffffff);
	xMOV(edx, lane_overflow);
	xForwardJump32 add_result_ready_from_overflow;
	add_underflow_result.SetTarget();
	xOR(eax, r9d);
	xMOV(edx, lane_underflow);
	xForwardJump32 add_result_ready_from_underflow;
	add_result_zero.SetTarget();
	xXOR(eax, eax);
	xXOR(edx, edx);
	xForwardJump32 add_result_ready_from_zero;

	add_acc_denormal.SetTarget();
	xTEST(edx, edx);
	xForwardJZ32 add_both_denormal;
	xForwardJump32 pack_product_result;
	add_both_denormal.SetTarget();
	xMOV(eax, ptr32[rsp + accumulator_raw]);
	xAND(eax, 0x80000000);
	xAND(eax, ptr32[rsp + product_sign]);
	xXOR(edx, edx);
	xForwardJump32 add_result_ready_from_both_denormal;
	add_product_denormal.SetTarget();
	add_result_acc_large_diff.SetTarget();
	xMOV(eax, ptr32[rsp + accumulator_raw]);
	xXOR(edx, edx);
	xForwardJump32 add_result_ready_from_acc;
	add_result_product_large_diff.SetTarget();
	pack_product_result.SetTarget();
	xMOV(eax, ptr32[rsp + product_significand]);
	xAND(eax, 0x7fffff);
	xMOV(edx, ptr32[rsp + product_exponent]);
	xSHL(edx, 23);
	xOR(eax, edx);
	xOR(eax, ptr32[rsp + product_sign]);
	xXOR(edx, edx);

	add_result_ready.SetTarget();
	add_result_ready_from_overflow.SetTarget();
	add_result_ready_from_underflow.SetTarget();
	add_result_ready_from_zero.SetTarget();
	add_result_ready_from_both_denormal.SetTarget();
	add_result_ready_from_acc.SetTarget();
	add_result_ready_from_acc_overflow.SetTarget();
	add_result_ready_from_mac_exception.SetTarget();
	xMOV(ecx, ptr32[rsp + product_status]);
	xADD(rsp, stack_size);
	xRET();
}

static void mVUGenerateSoftAddExactLaneKernel(microVU& mVU)
{
	// Internal ABI: eax = left raw value, edx = effective right raw value;
	// returns eax = exact result and edx bit 0/1 = overflow/underflow.
	constexpr sptr add_self = 0;
	constexpr int add_other = add_self + 4;
	constexpr int add_exponent = add_other + 4;
	constexpr int add_sign = add_exponent + 4;
	constexpr int add_shift = add_sign + 4;
	constexpr int lane_flags = add_shift + 4;
	constexpr int stack_size = 32;
	constexpr int lane_overflow = 1;
	constexpr int lane_underflow = 2;
	mVU.softAddExactLane = xGetAlignedCallTarget();
	xSUB(rsp, stack_size);
	xMOV(ptr32[rsp + add_self], eax);
	xMOV(ptr32[rsp + add_other], edx);
	xMOV(ptr32[rsp + lane_flags], 0);

	xMOV(ecx, eax);
	xSHR(ecx, 23);
	xAND(ecx, 0xff);
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
	xAND(ptr32[rsp + add_self], edx);
	xMOV(eax, ptr32[rsp + add_self]);
	xMOV(edx, ptr32[rsp + add_other]);
	xMOV(ptr32[rsp + add_self], edx);
	xMOV(ptr32[rsp + add_other], eax);
	xForwardJump32 add_operands_ready_from_other;

	add_truncate_other.SetTarget();
	xDEC(ecx);
	xMOV(edx, 0xffffffff);
	xSHL(edx, cl);
	xINC(ecx);
	xAND(ptr32[rsp + add_other], edx);

	add_operands_ready.SetTarget();
	add_operands_ready_from_other.SetTarget();
	xMOV(ptr32[rsp + add_shift], ecx);
	xMOV(eax, ptr32[rsp + add_self]);
	xMOV(edx, eax);
	xSHR(edx, 23);
	xAND(edx, 0xff);
	xMOV(ptr32[rsp + add_exponent], edx);
	xMOV(edx, eax);
	xSAR(edx, 31);
	xAND(eax, 0x7fffff);
	xOR(eax, 0x800000);
	xXOR(eax, edx);
	xSUB(eax, edx);
	xSHL(eax, 6);
	xMOV(ptr32[rsp + add_self], eax);

	xMOV(eax, ptr32[rsp + add_other]);
	xMOV(edx, eax);
	xSAR(edx, 31);
	xAND(eax, 0x7fffff);
	xOR(eax, 0x800000);
	xXOR(eax, edx);
	xSUB(eax, edx);
	xSHL(eax, 6);
	xMOV(ecx, ptr32[rsp + add_shift]);
	xSAR(eax, cl);
	xADD(eax, ptr32[rsp + add_self]);
	xMOV(edx, eax);
	xAND(edx, 0x80000000);
	xMOV(ptr32[rsp + add_sign], edx);
	xMOV(edx, eax);
	xSAR(edx, 31);
	xXOR(eax, edx);
	xSUB(eax, edx);
	xForwardJZ32 add_result_zero;

	xBSR(ecx, eax);
	xMOV(edx, ptr32[rsp + add_exponent]);
	xADD(edx, ecx);
	xSUB(edx, 29);
	xMOV(ptr32[rsp + add_exponent], edx);
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
	xMOV(edx, ptr32[rsp + add_exponent]);
	xCMP(edx, 255);
	xForwardJG32 add_overflow_result;
	xCMP(edx, 1);
	xForwardJL32 add_underflow_result;
	xSHL(edx, 23);
	xOR(eax, edx);
	xOR(eax, ptr32[rsp + add_sign]);
	xForwardJump32 add_result_ready;

	add_overflow_result.SetTarget();
	xMOV(eax, ptr32[rsp + add_sign]);
	xOR(eax, 0x7fffffff);
	xMOV(ptr32[rsp + lane_flags], lane_overflow);
	xForwardJump32 add_result_ready_from_overflow;

	add_underflow_result.SetTarget();
	xOR(eax, ptr32[rsp + add_sign]);
	xMOV(ptr32[rsp + lane_flags], lane_underflow);
	xForwardJump32 add_result_ready_from_underflow;

	add_result_zero.SetTarget();
	xXOR(eax, eax);
	xForwardJump32 add_result_ready_from_zero;

	add_self_denormal.SetTarget();
	xTEST(edx, edx);
	xForwardJZ32 add_both_denormal;
	xMOV(eax, ptr32[rsp + add_other]);
	xForwardJump32 add_result_ready_from_self_denormal;

	add_both_denormal.SetTarget();
	xMOV(eax, ptr32[rsp + add_self]);
	xAND(eax, 0x80000000);
	xMOV(edx, ptr32[rsp + add_other]);
	xAND(eax, edx);
	xForwardJump32 add_result_ready_from_both_denormal;

	add_other_denormal.SetTarget();
	add_result_self_large_diff.SetTarget();
	xMOV(eax, ptr32[rsp + add_self]);
	xForwardJump32 add_result_ready_from_self;

	add_result_other_large_diff.SetTarget();
	xMOV(eax, ptr32[rsp + add_other]);

	add_result_ready.SetTarget();
	add_result_ready_from_overflow.SetTarget();
	add_result_ready_from_underflow.SetTarget();
	add_result_ready_from_zero.SetTarget();
	add_result_ready_from_self_denormal.SetTarget();
	add_result_ready_from_both_denormal.SetTarget();
	add_result_ready_from_self.SetTarget();
	xMOV(edx, ptr32[rsp + lane_flags]);
	xADD(rsp, stack_size);
	xRET();
}

static void mVUGenerateSoftAddLaneRepairKernel(microVU& mVU)
{
	// Internal ABI: rdx points at the operation-local repair context. Preserve
	// every allocator-visible register except RDX, the only GPR the ADD emitter
	// declares clobbered. Source and operand are already lane-prepared and SUB's
	// right sign has already been inverted.
	constexpr sptr saved_xmm = 0;
	constexpr int cancellation_mask = saved_xmm + 5 * 16;
	constexpr int lane_flags = cancellation_mask + 16;
	// The six saved GPRs leave RSP 8-byte misaligned after the helper call's
	// return address.  Reserve an extra eight bytes so the MOVAPS XMM save area
	// and the cancellation temporary remain 16-byte aligned.
	constexpr int stack_size = 120;
	constexpr int lane_overflow = 1;
	constexpr int lane_underflow = 2;
	mVU.softAddLaneRepair = xGetAlignedCallTarget();
	xPUSH(rax);
	xPUSH(rcx);
	xPUSH(r8);
	xPUSH(r9);
	xPUSH(r10);
	xPUSH(r11);
	xSUB(rsp, stack_size);
	for (int reg = 0; reg < 5; reg++)
		xMOVAPS(ptr128[rsp + saved_xmm + reg * 16], xRegisterSSE(reg));
	xMOV(r11, rdx);
	xMOV(r10d, ptr32[r11 + VU_SOFT_ADD_REPAIR_BAD_MASK]);
	xMOV(r9d, ptr32[r11 + VU_SOFT_ADD_REPAIR_ACTIVE_MASK]);

	xMOVAPS(xmm0, ptr128[r11 + VU_SOFT_ADD_REPAIR_SOURCE]);
	xMOVAPS(xmm1, ptr128[r11 + VU_SOFT_ADD_REPAIR_OPERAND]);
	// Turn the architectural bad mask into an x86 lane mask and replace those
	// inputs with benign normals while the remaining lanes run in parallel.
	xMOV(eax, r10d);
	xMOV64(rdx, reinterpret_cast<uptr>(s_vu_soft_lane_mask));
	xMOVZX(eax, ptr8[xAddressVoid(rdx, rax, 1)]);
	xSHL(eax, 4);
	xMOV64(rdx, reinterpret_cast<uptr>(s_vu_soft_x86_lane_masks.data()));
	xMOVAPS(xmm4, ptr128[xAddressVoid(rdx, rax, 1)]);
	if (x86Emitter::use_avx)
	{
		xPBLEND.VB(xmm0, xmm0, ptr128[s_vu_soft_float_one], xmm4);
		xPBLEND.VB(xmm1, xmm1, ptr128[s_vu_soft_float_one], xmm4);
	}
	else
	{
		// SSE4 PBLENDVB has an implicit xmm0 mask. Preserve the first input in
		// xmm2 while xmm0 supplies the bad-lane mask to both blends.
		xMOVAPS(xmm2, xmm0);
		xMOVAPS(xmm0, xmm4);
		xPBLEND.VB(xmm2, xmm2, ptr128[s_vu_soft_float_one], xmm0);
		xPBLEND.VB(xmm1, xmm1, ptr128[s_vu_soft_float_one], xmm0);
		xMOVAPS(xmm0, xmm2);
	}

	xPSRL.D(xmm2, xmm0, 23);
	xPAND(xmm2, ptr128[s_vu_soft_exp_mask]);
	xPCMP.EQD(xmm4, xmm2, ptr128[s_vu_soft_zero]);
	xPAND(xmm4, ptr128[s_vu_soft_abs]);
	xPXOR(xmm4, ptr128[s_vu_soft_all_ones]);
	xPAND(xmm0, xmm4);
	xPSRL.D(xmm3, xmm1, 23);
	xPAND(xmm3, ptr128[s_vu_soft_exp_mask]);
	xPCMP.EQD(xmm4, xmm3, ptr128[s_vu_soft_zero]);
	xPAND(xmm4, ptr128[s_vu_soft_abs]);
	xPXOR(xmm4, ptr128[s_vu_soft_all_ones]);
	xPAND(xmm1, xmm4);

	xPSUB.D(xmm2, xmm3);
	xPABS.D(xmm3, xmm2);
	xPCMP.GTD(xmm4, xmm3, ptr128[s_vu_soft_exp_24]);
	if (x86Emitter::use_avx)
	{
		xPBLEND.VB(xmm3, xmm3, ptr128[s_vu_soft_exp_33], xmm4);
	}
	else
	{
		// cancellation_mask is still free here and is aligned for MOVAPS.
		xMOVAPS(ptr128[rsp + cancellation_mask], xmm0);
		xMOVAPS(xmm0, xmm4);
		xPBLEND.VB(xmm3, xmm3, ptr128[s_vu_soft_exp_33], xmm0);
		xMOVAPS(xmm0, ptr128[rsp + cancellation_mask]);
	}
	xPSUB.D(xmm3, ptr128[s_vu_soft_one]);
	xPMAX.SD(xmm3, ptr128[s_vu_soft_zero]);
	xPCMP.GTD(xmm4, xmm2, ptr128[s_vu_soft_zero]);
	if (x86Emitter::use_avx)
	{
		xPBLEND.VB(xmm2, xmm1, xmm0, xmm4);
		xPBLEND.VB(xmm1, xmm0, xmm1, xmm4);
	}
	else
	{
		// Save the first term while xmm0 is the implicit mask, then form the
		// larger and smaller terms without destroying either source.
		xMOVAPS(ptr128[rsp + cancellation_mask], xmm0);
		xMOVAPS(xmm0, xmm4);
		xMOVAPS(xmm2, xmm1);
		xPBLEND.VB(xmm2, xmm2, ptr128[rsp + cancellation_mask], xmm0);
		xMOVAPS(xmm4, ptr128[rsp + cancellation_mask]);
		xPBLEND.VB(xmm4, xmm4, xmm1, xmm0);
		xMOVAPS(xmm1, xmm4);
	}
	xVPSRLVD(xmm1, xmm1, xmm3);
	xVPSLLVD(xmm1, xmm1, xmm3);
	xMOVAPS(xmm0, xmm2);

	// A native zero is valid only for exact cancellation or two PS2-zero terms.
	xPXOR(xmm4, xmm1, ptr128[s_vu_soft_sign]);
	xPCMP.EQD(xmm4, xmm0);
	xPAND(xmm2, xmm0, ptr128[s_vu_soft_abs]);
	xPCMP.EQD(xmm2, ptr128[s_vu_soft_zero]);
	xPAND(xmm3, xmm1, ptr128[s_vu_soft_abs]);
	xPCMP.EQD(xmm3, ptr128[s_vu_soft_zero]);
	xPAND(xmm2, xmm3);
	xPOR(xmm4, xmm2);
	xMOVAPS(ptr128[rsp + cancellation_mask], xmm4);

	// The helper is entered only from a cold packed repair edge. Always use the
	// exact truncate mode here; zero lanes were canonicalized explicitly and a
	// true underflow is detected below before any native result is accepted.
	xLDMXCSR(ptr32[&s_vu_soft_truncate_mxcsr]);
	xADD.PS(xmm0, xmm1);
	xLDMXCSR(ptr32[mVU.index == 0 ? &EmuConfig.Cpu.VU0FPCR.bitmask : &EmuConfig.Cpu.VU1FPCR.bitmask]);

	xPAND(xmm1, xmm0, ptr128[s_vu_soft_abs]);
	xPCMP.GTD(xmm3, xmm1, ptr128[s_vu_soft_max_safe]);
	xMOVAPS(xmm4, ptr128[rsp + cancellation_mask]);
	xMOVAPS(xmm2, ptr128[s_vu_soft_hidden_bit]);
	xPCMP.GTD(xmm2, xmm1);
	xPANDN(xmm4, xmm2);
	xPOR(xmm3, xmm4);
	xMOVMSKPS(eax, xmm3);
	xMOV64(rdx, reinterpret_cast<uptr>(s_vu_soft_lane_mask));
	xMOVZX(eax, ptr8[xAddressVoid(rdx, rax, 1)]);
	xAND(eax, r9d);
	xOR(r10d, eax);
	xMOVAPS(ptr128[r11 + VU_SOFT_RESULT_VALUE_OFFSET], xmm0);

	for (const size_t offset : {
			 offsetof(VuSoftFmacJitResult, mac_flags),
			 offsetof(VuSoftFmacJitResult, status_flags),
			 offsetof(VuSoftFmacJitResult, acc_overflow_mask)})
	{
		xMOV(ptr32[r11 + offset], 0);
	}
	for (int lane = 0; lane < 4; lane++)
	{
		const u32 lane_bit = 8u >> lane;
		const u32 lane_shift = 3 - lane;
		xTEST(r9d, lane_bit);
		xForwardJZ32 lane_finished;
		xXOR(r8d, r8d);
		xTEST(r10d, lane_bit);
		xForwardJZ32 lane_native;
		xMOV(eax, ptr32[r11 + VU_SOFT_ADD_REPAIR_SOURCE + lane * 4]);
		xMOV(edx, ptr32[r11 + VU_SOFT_ADD_REPAIR_OPERAND + lane * 4]);
		xCALL(mVU.softAddExactLane);
		xMOV(r8d, edx);
		xMOV(ptr32[r11 + VU_SOFT_RESULT_VALUE_OFFSET + lane * 4], eax);
		xForwardJump32 lane_result_ready;
		lane_native.SetTarget();
		xMOV(eax, ptr32[r11 + VU_SOFT_RESULT_VALUE_OFFSET + lane * 4]);
		lane_result_ready.SetTarget();
		xMOV(ptr32[rsp + lane_flags], r8d);

		xTEST(eax, 0x80000000);
		xForwardJZ8 result_positive;
		xOR(ptr32[r11 + offsetof(VuSoftFmacJitResult, mac_flags)], 0x0010u << lane_shift);
		result_positive.SetTarget();
		xTEST(ptr32[rsp + lane_flags], lane_underflow);
		xForwardJZ8 result_not_underflow;
		xOR(ptr32[r11 + offsetof(VuSoftFmacJitResult, mac_flags)], 0x0101u << lane_shift);
		xForwardJump32 result_flags_ready;
		result_not_underflow.SetTarget();
		xMOV(edx, eax);
		xAND(edx, 0x7fffffff);
		xForwardJNZ8 result_not_zero;
		xOR(ptr32[r11 + offsetof(VuSoftFmacJitResult, mac_flags)], 0x0001u << lane_shift);
		xForwardJump32 result_flags_ready_from_zero;
		result_not_zero.SetTarget();
		xTEST(ptr32[rsp + lane_flags], lane_overflow);
		xForwardJZ8 result_flags_ready_nonexception;
		xOR(ptr32[r11 + offsetof(VuSoftFmacJitResult, mac_flags)], 0x1000u << lane_shift);
		xOR(ptr32[r11 + offsetof(VuSoftFmacJitResult, acc_overflow_mask)], lane_bit);
		result_flags_ready.SetTarget();
		result_flags_ready_from_zero.SetTarget();
		result_flags_ready_nonexception.SetTarget();
		lane_finished.SetTarget();
	}
	mVUemitUpperStatusFromMacFlags(
		ptr32[r11 + offsetof(VuSoftFmacJitResult, mac_flags)],
		ptr32[r11 + offsetof(VuSoftFmacJitResult, status_flags)]);

	for (int reg = 0; reg < 5; reg++)
		xMOVAPS(xRegisterSSE(reg), ptr128[rsp + saved_xmm + reg * 16]);
	xADD(rsp, stack_size);
	xPOP(r11);
	xPOP(r10);
	xPOP(r9);
	xPOP(r8);
	xPOP(rcx);
	xPOP(rax);
	xRET();
}

static void mVUGenerateSoftMulExactVectorKernel(microVU& mVU)
{
	// Internal ABI: rax = source, rdx = operand, rcx = result, r8d = VU lane mask.
	constexpr sptr source_ptr = 0;
	constexpr int operand_ptr = source_ptr + 8;
	constexpr int result_ptr = operand_ptr + 8;
	constexpr int lane_mask = result_ptr + 8;
	constexpr int stack_size = 40;
	constexpr int product_underflow = 1;
	constexpr int product_overflow = 2;
	mVU.softMulExactVector = xGetAlignedCallTarget();
	xSUB(rsp, stack_size);
	xMOV(ptr64[rsp + source_ptr], rax);
	xMOV(ptr64[rsp + operand_ptr], rdx);
	xMOV(ptr64[rsp + result_ptr], rcx);
	xMOV(ptr32[rsp + lane_mask], r8d);
	xMOV(r10, rcx);
	for (const size_t offset : {
			 offsetof(VuSoftFmacJitResult, mac_flags),
			 offsetof(VuSoftFmacJitResult, status_flags),
			 offsetof(VuSoftFmacJitResult, acc_overflow_mask)})
	{
		xMOV(ptr32[r10 + offset], 0);
	}
	// Masked COP2 ACC writeback folds this into sticky status even for MUL.
	xMOV(ptr32[r10 + offsetof(VuSoftFmacJitResult, mul_stage_status_flags)], 0);

	for (int lane = 0; lane < 4; lane++)
	{
		const u32 lane_bit = 8u >> lane;
		const u32 shift = 3 - lane;
		xTEST(ptr32[rsp + lane_mask], lane_bit);
		xForwardJZ32 lane_finished;
		xMOV(rax, ptr64[rsp + source_ptr]);
		xMOV(eax, ptr32[rax + lane * 4]);
		xMOV(rdx, ptr64[rsp + operand_ptr]);
		xMOV(edx, ptr32[rdx + lane * 4]);
		xCALL(mVU.softMulExact);
		xMOV(r10, ptr64[rsp + result_ptr]);
		xMOV(ptr32[r10 + VU_SOFT_RESULT_VALUE_OFFSET + lane * 4], eax);
		xTEST(eax, 0x80000000);
		xForwardJZ8 result_positive;
		xOR(ptr32[r10 + offsetof(VuSoftFmacJitResult, mac_flags)], 0x0010u << shift);
		xOR(ptr32[r10 + offsetof(VuSoftFmacJitResult, status_flags)], 0x2);
		result_positive.SetTarget();
		xTEST(edx, product_underflow);
		xForwardJZ8 result_not_underflow;
		xOR(ptr32[r10 + offsetof(VuSoftFmacJitResult, mac_flags)], 0x0101u << shift);
		xOR(ptr32[r10 + offsetof(VuSoftFmacJitResult, status_flags)], 0x5);
		xForwardJump32 result_flags_ready;
		result_not_underflow.SetTarget();
		xMOV(ecx, eax);
		xAND(ecx, 0x7fffffff);
		xForwardJNZ8 result_not_zero;
		xOR(ptr32[r10 + offsetof(VuSoftFmacJitResult, mac_flags)], 0x0001u << shift);
		xOR(ptr32[r10 + offsetof(VuSoftFmacJitResult, status_flags)], 0x1);
		xForwardJump32 result_flags_ready_from_zero;
		result_not_zero.SetTarget();
		xTEST(edx, product_overflow);
		xForwardJZ8 result_flags_ready_nonexception;
		xOR(ptr32[r10 + offsetof(VuSoftFmacJitResult, mac_flags)], 0x1000u << shift);
		xOR(ptr32[r10 + offsetof(VuSoftFmacJitResult, status_flags)], 0x8);
		xOR(ptr32[r10 + offsetof(VuSoftFmacJitResult, acc_overflow_mask)], lane_bit);
		result_flags_ready.SetTarget();
		result_flags_ready_from_zero.SetTarget();
		result_flags_ready_nonexception.SetTarget();
		lane_finished.SetTarget();
	}
	xADD(rsp, stack_size);
	xRET();
}

static void mVUGenerateSoftMulBoothPackedKernel(microVU& mVU)
{
	constexpr sptr saved_xmm = 0;
	constexpr int full_lo = saved_xmm + 5 * 16;
	constexpr int full_hi = full_lo + 16;
	constexpr int mantissa_a = full_hi + 16;
	constexpr int mantissa_b = mantissa_a + 16;
	constexpr int booth_negate = mantissa_b + 16;
	constexpr int booth_data = booth_negate + 8 * 16;
	constexpr int add3_values = booth_data + 8 * 16;
	constexpr int stack_size = add3_values + 12 * 16;
	const void* const uncached_kernel = xGetAlignedCallTarget();
	xSUB(rsp, stack_size);
	for (int reg = 0; reg < 5; reg++)
		xMOVAPS(ptr128[rsp + saved_xmm + reg * 16], xRegisterSSE(reg));

	xMOVAPS(xmm0, ptr128[rax]);
	xMOVAPS(xmm1, ptr128[rdx]);
	xPSHUF.D(xmm2, xmm0, 0xf5);
	xPSHUF.D(xmm3, xmm1, 0xf5);
	xPAND(xmm0, ptr128[s_vu_soft_mantissa]);
	xPOR(xmm0, ptr128[s_vu_soft_hidden_bit]);
	xPAND(xmm1, ptr128[s_vu_soft_mantissa]);
	xPOR(xmm1, ptr128[s_vu_soft_hidden_bit]);
	xMOVAPS(ptr128[rsp + mantissa_a], xmm0);
	xMOVAPS(ptr128[rsp + mantissa_b], xmm1);
	xPAND(xmm2, ptr128[s_vu_soft_mantissa]);
	xPOR(xmm2, ptr128[s_vu_soft_hidden_bit]);
	xPAND(xmm3, ptr128[s_vu_soft_mantissa]);
	xPOR(xmm3, ptr128[s_vu_soft_hidden_bit]);
	xPMUL.UDQ(xmm0, xmm0, xmm1);
	xPMUL.UDQ(xmm2, xmm2, xmm3);
	xMOVAPS(xmm4, xmm0);
	xMOVAPS(xmm3, xmm2);
	xPSHUF.D(xmm4, xmm4, 0xfd);
	xPSHUF.D(xmm3, xmm3, 0xfd);
	xPUNPCK.LDQ(xmm4, xmm3);
	xMOVAPS(ptr128[rsp + full_hi], xmm4);
	xPSHUF.D(xmm0, xmm0, 0xa8);
	xPSHUF.D(xmm2, xmm2, 0xa8);
	xPUNPCK.LDQ(xmm0, xmm2);
	xMOVAPS(ptr128[rsp + full_lo], xmm0);

	for (int bit = 0; bit < 8; bit++)
	{
		const int shift = bit * 2;
		xMOVAPS(xmm0, ptr128[rsp + mantissa_a]);
		if (shift != 0)
			xPSLL.D(xmm0, shift);

		xMOVAPS(xmm1, ptr128[rsp + mantissa_b]);
		if (bit == 0)
			xPSLL.D(xmm1, 1);
		else
			xPSRL.D(xmm1, shift - 1);
		xPAND(xmm1, ptr128[s_vu_soft_magnitude]);

		xMOVAPS(xmm2, ptr128[s_vu_soft_booth_decode_bytes]);
		xPSHUF.B(xmm2, xmm1);
		xMOVAPS(xmm3, xmm2);
		xPAND(xmm3, ptr128[s_vu_soft_booth_magnitude]);
		xPMUL.LD(xmm0, xmm3);
		xPSRL.D(xmm2, 4);
		if (shift != 0)
			xPSLL.D(xmm2, shift);
		xMOVAPS(ptr128[rsp + booth_negate + bit * 16], xmm2);
		xPXOR(xmm4, xmm4);
		xPSUB.D(xmm4, xmm2);
		xPXOR(xmm0, xmm4);
		xMOVAPS(ptr128[rsp + booth_data + bit * 16], xmm0);
	}

	const auto emitAdd3 = [&](int a, int b, int c, int lo, int hi) {
		xMOVAPS(xmm0, ptr128[rsp + a]);
		xPXOR(xmm0, ptr128[rsp + b]);
		xMOVAPS(xmm1, xmm0);
		xPAND(xmm1, ptr128[rsp + c]);
		xPXOR(xmm0, ptr128[rsp + c]);
		xMOVAPS(ptr128[rsp + lo], xmm0);
		xMOVAPS(xmm2, ptr128[rsp + a]);
		xPAND(xmm2, ptr128[rsp + b]);
		xPOR(xmm2, xmm1);
		xPSLL.D(xmm2, 1);
		xMOVAPS(ptr128[rsp + hi], xmm2);
	};
	constexpr int t0_lo = add3_values + 0 * 16;
	constexpr int t0_hi = add3_values + 1 * 16;
	constexpr int t1_lo = add3_values + 2 * 16;
	constexpr int t1_hi = add3_values + 3 * 16;
	constexpr int t2_lo = add3_values + 4 * 16;
	constexpr int t2_hi = add3_values + 5 * 16;
	constexpr int t3_lo = add3_values + 6 * 16;
	constexpr int t3_hi = add3_values + 7 * 16;
	constexpr int t4_lo = add3_values + 8 * 16;
	constexpr int t4_hi = add3_values + 9 * 16;
	constexpr int t5_lo = add3_values + 10 * 16;
	constexpr int t5_hi = add3_values + 11 * 16;

	emitAdd3(booth_data + 1 * 16, booth_data + 2 * 16, booth_data + 3 * 16, t0_lo, t0_hi);
	xMOVAPS(xmm0, ptr128[rsp + booth_data + 4 * 16]);
	xPAND(xmm0, ptr128[s_vu_soft_low_11_mask]);
	xMOVAPS(ptr128[rsp + booth_data + 4 * 16], xmm0);
	xMOVAPS(xmm0, ptr128[rsp + booth_data + 5 * 16]);
	xMOVAPS(ptr128[rsp + mantissa_a], xmm0);
	xPAND(xmm0, ptr128[s_vu_soft_low_12_mask]);
	xMOVAPS(ptr128[rsp + booth_data + 5 * 16], xmm0);
	emitAdd3(booth_data + 4 * 16, booth_data + 5 * 16, booth_data + 6 * 16, t1_lo, t1_hi);
	xMOVAPS(xmm0, ptr128[rsp + mantissa_a]);
	xPAND(xmm0, ptr128[s_vu_soft_bit_11]);
	xPOR(xmm0, ptr128[rsp + booth_negate + 6 * 16]);
	xMOVAPS(xmm1, ptr128[rsp + t1_hi]);
	xPOR(xmm1, xmm0);
	xMOVAPS(ptr128[rsp + t1_hi], xmm1);
	xMOVAPS(xmm0, ptr128[rsp + mantissa_a]);
	xPAND(xmm0, ptr128[s_vu_soft_bit_10]);
	xPADD.D(xmm0, ptr128[rsp + booth_negate + 5 * 16]);
	xMOVAPS(xmm1, ptr128[rsp + booth_data + 7 * 16]);
	xPOR(xmm1, xmm0);
	xMOVAPS(ptr128[rsp + booth_data + 7 * 16], xmm1);
	emitAdd3(booth_data + 0 * 16, t0_lo, t0_hi, t2_lo, t2_hi);
	emitAdd3(booth_data + 7 * 16, t1_lo, t1_hi, t3_lo, t3_hi);
	emitAdd3(t2_hi, t3_lo, t3_hi, t4_lo, t4_hi);
	emitAdd3(t2_lo, t4_lo, t4_hi, t5_lo, t5_hi);
	xMOVAPS(xmm0, ptr128[rsp + t5_hi]);
	xPADD.D(xmm0, ptr128[rsp + booth_negate + 7 * 16]);
	xPAND(xmm0, ptr128[s_vu_soft_low_15_mask]);
	xMOVAPS(xmm1, ptr128[rsp + t5_lo]);
	xPAND(xmm1, ptr128[s_vu_soft_low_15_mask]);
	xPADD.D(xmm0, xmm1);
	xPXOR(xmm0, ptr128[rsp + full_lo]);
	xPAND(xmm0, ptr128[s_vu_soft_bit_15]);
	xPSRL.D(xmm0, 15);
	xMOVAPS(xmm1, ptr128[rsp + full_lo]);
	xPAND(xmm1, ptr128[s_vu_soft_mantissa]);
	xMOVAPS(xmm2, ptr128[s_vu_soft_bit_15]);
	xPCMP.GTD(xmm2, xmm1);
	xPAND(xmm0, xmm2);
	// A normalized mantissa product truncates at bit 24. In that domain, bit 23
	// must also be clear before subtracting Booth's bit-15 correction changes
	// the encoded float result.
	xMOVAPS(xmm1, ptr128[rsp + full_lo]);
	xPAND(xmm1, ptr128[s_vu_soft_hidden_bit]);
	xPCMP.EQD(xmm1, ptr128[s_vu_soft_hidden_bit]);
	xMOVAPS(xmm2, ptr128[rsp + full_hi]);
	xPAND(xmm2, ptr128[s_vu_soft_bit_15]);
	xPCMP.EQD(xmm2, ptr128[s_vu_soft_bit_15]);
	xPAND(xmm1, xmm2);
	xPXOR(xmm1, ptr128[s_vu_soft_all_ones]);
	xPAND(xmm0, xmm1);
	// The packed Booth tree has a distinct boundary when the first mantissa is
	// exactly one. Use the exhaustive low-half table for those lanes.
	xMOVAPS(xmm4, ptr128[rax]);
	xPAND(xmm4, ptr128[s_vu_soft_mantissa]);
	xPCMP.EQD(xmm4, ptr128[s_vu_soft_zero]);
	xMOVMSKPS(r10d, xmm4);
	xTEST(r10d, r10d);
	xForwardJZ32 packed_first_one_ready;
	xPXOR(xmm3, xmm3);
	xMOV64(r11, reinterpret_cast<uptr>(MicroVUSoftFloatTables::first_one_correction_lookup));
	for (int lane = 0; lane < 4; lane++)
	{
		xMOV(r10d, ptr32[rdx + lane * 4]);
		xAND(r10d, 0xffff);
		xMOVZX(r10d, ptr8[xAddressVoid(r11, r10, 1)]);
		xPINSR.D(xmm3, r10d, lane);
	}
	if (x86Emitter::use_avx)
	{
		xPBLEND.VB(xmm0, xmm0, xmm3, xmm4);
	}
	else
	{
		// SSE4 PBLENDVB takes its mask implicitly from xmm0. Preserve the old
		// correction vector while moving the first-one lane mask into place.
		xMOVAPS(xmm1, xmm0);
		xMOVAPS(xmm0, xmm4);
		xPBLEND.VB(xmm1, xmm1, xmm3, xmm0);
		xMOVAPS(xmm0, xmm1);
	}
	packed_first_one_ready.SetTarget();
	// The cache key is mantissa-only, so its value must be too. Callers exclude
	// PS2-zero lanes and reject extended inputs before accepting the product.
	xMOVAPS(ptr128[rcx], xmm0);

	for (int reg = 0; reg < 5; reg++)
		xMOVAPS(xRegisterSSE(reg), ptr128[rsp + saved_xmm + reg * 16]);
	xADD(rsp, stack_size);
	xRET();

	mVU.softMulBoothPacked = xGetAlignedCallTarget();
	std::array<std::optional<xForwardJump32>, 8> cache_misses;
	constexpr sptr cache_saved_r8 = 0;
	constexpr int cache_saved_r9 = cache_saved_r8 + 8;
	constexpr int cache_lane_mask = cache_saved_r9 + 8;
	constexpr int cache_stack_size = 32;
	xSUB(rsp, cache_stack_size);
	xMOV(ptr64[rsp + cache_saved_r8], r8);
	xMOV(ptr64[rsp + cache_saved_r9], r9);
	xMOV(ptr32[rsp + cache_lane_mask], r10d);
	xMOV(ptr64[rcx], 0);
	xMOV(ptr64[rcx + 8], 0);
	xMOV64(r11, reinterpret_cast<uptr>(mVU.softBoothCache.get()));
	for (int lane = 0; lane < 4; lane++)
	{
		xTEST(ptr32[rsp + cache_lane_mask], 1u << lane);
		xForwardJZ32 cache_lane_ready;
		xMOV(r8d, ptr32[rax + lane * 4]);
		xAND(r8d, 0x7fffff);
		xMOV(r10d, ptr32[rdx + lane * 4]);
		xAND(r10d, 0x7fffff);
		// The key's high dword is operand_mantissa >> 9. Compute it in
		// parallel with the low-dword key construction to shorten the
		// dependency chain into the random cache access.
		xMOV(r9d, r10d);
		xSHR(r9d, 9);
		xSHL(r10, 23);
		xOR(r8, r10);
		xXOR(r9d, r8d);
		xAND(r9d, mVUsoftBoothCacheSize - 1);
		xSHL(r9, 4);
		xCMP(r8, ptr64[xAddressVoid(r11, r9, 1)]);
		cache_misses[lane * 2].emplace(Jcc_NotEqual);
		xMOV(r10d, ptr32[xAddressVoid(r11, r9, 1,
					   offsetof(microVUSoftBoothCacheEntry, correction_with_valid_bit))]);
		xTEST(r10d, 0x80000000);
		cache_misses[lane * 2 + 1].emplace(Jcc_Zero);
		xAND(r10d, 1);
		xMOV(ptr32[rcx + lane * 4], r10d);
		cache_lane_ready.SetTarget();
	}
	xMOV(r8, ptr64[rsp + cache_saved_r8]);
	xMOV(r9, ptr64[rsp + cache_saved_r9]);
	xADD(rsp, cache_stack_size);
	xRET();

	for (std::optional<xForwardJump32>& miss : cache_misses)
		miss->SetTarget();
	xCALL(uncached_kernel);
	xMOV64(r11, reinterpret_cast<uptr>(mVU.softBoothCache.get()));
	for (int lane = 0; lane < 4; lane++)
	{
		xTEST(ptr32[rsp + cache_lane_mask], 1u << lane);
		xForwardJZ32 cache_fill_lane_inactive;
		xMOV(r8d, ptr32[rax + lane * 4]);
		xAND(r8d, 0x7fffff);
		xMOV(r10d, ptr32[rdx + lane * 4]);
		xAND(r10d, 0x7fffff);
		xMOV(r9d, r10d);
		xSHR(r9d, 9);
		xSHL(r10, 23);
		xOR(r8, r10);
		xXOR(r9d, r8d);
		xAND(r9d, mVUsoftBoothCacheSize - 1);
		xSHL(r9, 4);
		xMOV(ptr64[xAddressVoid(r11, r9, 1)], r8);
		xMOV(r10d, ptr32[rcx + lane * 4]);
		xOR(r10d, 0x80000000);
		xMOV(ptr32[xAddressVoid(r11, r9, 1,
				 offsetof(microVUSoftBoothCacheEntry, correction_with_valid_bit))],
			r10d);
		xForwardJump32 cache_fill_lane_ready;
		cache_fill_lane_inactive.SetTarget();
		xMOV(ptr32[rcx + lane * 4], 0);
		cache_fill_lane_ready.SetTarget();
	}
	xMOV(r8, ptr64[rsp + cache_saved_r8]);
	xMOV(r9, ptr64[rsp + cache_saved_r9]);
	xADD(rsp, cache_stack_size);
	xRET();
}

static void mVUGenerateSoftMaddPackedKernels(microVU& mVU)
{
	constexpr sptr saved_xmm = 0;
	constexpr int source_values = saved_xmm + 5 * 16;
	constexpr int operand_values = source_values + 16;
	constexpr int accumulator_values = operand_values + 16;
	constexpr int product_values = accumulator_values + 16;
	constexpr int booth_correction = product_values + 16;
	constexpr int booth_lane_mask = booth_correction + 16;
	constexpr int native_lane_mask = booth_lane_mask + 4;
	constexpr int scratch_end = native_lane_mask + 4;
	constexpr int stack_size = ((scratch_end + 15) & ~15) + 8;
	for (int subtract = 0; subtract < 2; subtract++)
	{
		mVU.softMaddPacked[subtract] = xGetAlignedCallTarget();
		xSUB(rsp, stack_size);
		for (int reg = 0; reg < 5; reg++)
			xMOVAPS(ptr128[rsp + saved_xmm + reg * 16], xRegisterSSE(reg));

		// r10d carries the VU write mask and r11d carries the corresponding native lane
		// mask. Keep the native mask in a register on the common path; spill it only for
		// the uncommon Booth-correction helper call, which may clobber r11.
		xTEST(r8d, r10d);
		xForwardJNZ32 fail_acc_overflow;

		xMOVAPS(xmm0, ptr128[rax]);
		xMOVAPS(ptr128[rsp + source_values], xmm0);
		xMOVAPS(xmm1, ptr128[rdx]);
		xMOVAPS(ptr128[rsp + operand_values], xmm1);
		xMOVAPS(xmm2, ptr128[rcx]);
		xMOVAPS(ptr128[rsp + accumulator_values], xmm2);

		// This shared entry handles only the normal packed domain. The operation-local
		// lowering remains the exact fallback for denormals and extended exponents.
		xMOVAPS(xmm3, xmm0);
		xPSRL.D(xmm3, 23);
		xPAND(xmm3, ptr128[s_vu_soft_exp_mask]);
		xMOVAPS(xmm4, xmm3);
		xPCMP.EQD(xmm3, ptr128[s_vu_soft_zero]);
		xPCMP.EQD(xmm4, ptr128[s_vu_soft_exp_255]);
		xPOR(xmm3, xmm4);

		xMOVAPS(xmm4, xmm1);
		xPSRL.D(xmm4, 23);
		xPAND(xmm4, ptr128[s_vu_soft_exp_mask]);
		xMOVAPS(xmm0, xmm4);
		xPCMP.EQD(xmm4, ptr128[s_vu_soft_zero]);
		xPCMP.EQD(xmm0, ptr128[s_vu_soft_exp_255]);
		xPOR(xmm3, xmm4);
		xPOR(xmm3, xmm0);

		xMOVAPS(xmm4, xmm2);
		xPSRL.D(xmm4, 23);
		xPAND(xmm4, ptr128[s_vu_soft_exp_mask]);
		xMOVAPS(xmm0, xmm4);
		xPCMP.EQD(xmm4, ptr128[s_vu_soft_zero]);
		xPCMP.EQD(xmm0, ptr128[s_vu_soft_exp_255]);
		xPOR(xmm3, xmm4);
		xPOR(xmm3, xmm0);
		xMOVMSKPS(eax, xmm3);
		xAND(eax, r11d);
		xForwardJNZ32 fail_input_domain;

		// Identify products where the PS2 Booth tree can borrow into retained bit 15.
		xMOVAPS(xmm0, ptr128[rsp + source_values]);
		xMOVAPS(xmm1, ptr128[rsp + operand_values]);
		xMOVAPS(xmm4, xmm1);
		xPAND(xmm4, ptr128[s_vu_soft_mantissa]);
		xPCMP.EQD(xmm4, ptr128[s_vu_soft_zero]);
		xMOVAPS(xmm0, ptr128[rsp + source_values]);
		xPMUL.LD(xmm0, xmm0, ptr128[rsp + operand_values]);
		xPAND(xmm0, ptr128[s_vu_soft_mantissa]);
		xPCMP.GTD(xmm0, ptr128[s_vu_soft_borrow_limit]);
		xPOR(xmm0, xmm4);
		xMOVMSKPS(eax, xmm0);
		xAND(eax, r11d);
		xXOR(eax, r11d);
		xMOV(ptr32[rsp + booth_lane_mask], eax);

		xMOVAPS(xmm0, ptr128[rsp + source_values]);
		xMUL.PS(xmm0, ptr128[rsp + operand_values]);
		xMOVAPS(ptr128[rsp + product_values], xmm0);
		xMOVAPS(xmm4, xmm0);
		xPAND(xmm4, ptr128[s_vu_soft_abs]);
		xMOVAPS(xmm3, ptr128[s_vu_soft_hidden_bit]);
		xPCMP.GTD(xmm3, xmm4);
		xPCMP.GTD(xmm4, ptr128[s_vu_soft_max_safe]);
		xPOR(xmm3, xmm4);
		xMOVMSKPS(eax, xmm3);
		xAND(eax, r11d);
		xForwardJNZ32 fail_product_domain;

		xCMP(ptr32[rsp + booth_lane_mask], 0);
		xForwardJZ32 product_ready;
		xLEA(rax, ptr[rsp + source_values]);
		xLEA(rdx, ptr[rsp + operand_values]);
		xLEA(rcx, ptr[rsp + booth_correction]);
		xMOV(r10d, ptr32[rsp + booth_lane_mask]);
		xMOV(ptr32[rsp + native_lane_mask], r11d);
		xCALL(mVU.softMulBoothPacked);
		xMOVAPS(xmm0, ptr128[rsp + product_values]);
		xMOV(eax, ptr32[rsp + booth_lane_mask]);
		xSHL(eax, 4);
		xMOV64(r11, reinterpret_cast<uptr>(s_vu_soft_x86_lane_masks.data()));
		xMOVAPS(xmm2, ptr128[xAddressVoid(r11, rax, 1)]);
		xMOV(r11d, ptr32[rsp + native_lane_mask]);
		xPAND(xmm2, ptr128[rsp + booth_correction]);
		xPSUB.D(xmm0, xmm2);
		xMOVAPS(ptr128[rsp + product_values], xmm0);
		xMOVAPS(xmm4, xmm0);
		xPAND(xmm4, ptr128[s_vu_soft_abs]);
		xMOVAPS(xmm3, ptr128[s_vu_soft_hidden_bit]);
		xPCMP.GTD(xmm3, xmm4);
		xPCMP.GTD(xmm4, ptr128[s_vu_soft_max_safe]);
		xPOR(xmm3, xmm4);
		xMOVMSKPS(eax, xmm3);
		xAND(eax, r11d);
		xForwardJNZ32 fail_corrected_product_domain;
		product_ready.SetTarget();

		xMOVMSKPS(eax, xmm0);
		xAND(eax, r11d);
		xXOR(edx, edx);
		xTEST(eax, eax);
		xSETNZ(dl);
		xSHL(edx, 1);
		xMOV(ptr32[r9 + offsetof(VuSoftFmacJitResult, mul_stage_status_flags)], edx);
		if (subtract)
			xPXOR(xmm0, ptr128[s_vu_soft_sign]);

		xMOVAPS(xmm1, ptr128[rsp + accumulator_values]);
		xMOVAPS(xmm2, xmm1);
		xPSRL.D(xmm2, 23);
		xPAND(xmm2, ptr128[s_vu_soft_exp_mask]);
		xMOVAPS(xmm4, xmm0);
		xPSRL.D(xmm4, 23);
		xPAND(xmm4, ptr128[s_vu_soft_exp_mask]);
		xPSUB.D(xmm2, xmm4);

		xMOVAPS(xmm3, xmm2);
		xPABS.D(xmm3, xmm3);
		xMOVAPS(xmm4, xmm3);
		xPCMP.GTD(xmm4, ptr128[s_vu_soft_exp_24]);
		xMOVMSKPS(eax, xmm4);
		xAND(eax, r11d);
		xForwardJNZ32 fail_exponent_difference;

		xPSUB.D(xmm3, ptr128[s_vu_soft_one]);
		xPMAX.SD(xmm3, ptr128[s_vu_soft_zero]);
		xMOVAPS(xmm4, xmm2);
		xPCMP.GTD(xmm4, ptr128[s_vu_soft_zero]);
		xMOVAPS(xmm2, xmm1);
		xPBLEND.VB(xmm2, xmm1, xmm0, xmm4);
		xPBLEND.VB(xmm0, xmm0, xmm1, xmm4);
		xVPSRLVD(xmm2, xmm2, xmm3);
		xVPSLLVD(xmm3, xmm2, xmm3);
		xADD.PS(xmm0, xmm3);

		xMOVAPS(xmm4, xmm0);
		xPAND(xmm4, ptr128[s_vu_soft_abs]);
		xMOVAPS(xmm3, ptr128[s_vu_soft_hidden_bit]);
		xPCMP.GTD(xmm3, xmm4);
		xPCMP.GTD(xmm4, ptr128[s_vu_soft_max_safe]);
		xPOR(xmm3, xmm4);
		xMOVMSKPS(eax, xmm3);
		xAND(eax, r11d);
		xForwardJNZ32 fail_result_domain;

		xMOVAPS(ptr128[r9 + VU_SOFT_RESULT_VALUE_OFFSET], xmm0);
		xMOV(eax, 1);
		xForwardJump32 kernel_finished;

		fail_acc_overflow.SetTarget();
		fail_input_domain.SetTarget();
		fail_product_domain.SetTarget();
		fail_corrected_product_domain.SetTarget();
		fail_exponent_difference.SetTarget();
		fail_result_domain.SetTarget();
		xXOR(eax, eax);
		kernel_finished.SetTarget();
		for (int reg = 0; reg < 5; reg++)
			xMOVAPS(xRegisterSSE(reg), ptr128[rsp + saved_xmm + reg * 16]);
		xADD(rsp, stack_size);
		xRET();
	}
}


static void mVUGenerateSoftMaddExactVectorKernels(microVU& mVU)
{
	// Internal ABI: rax = source, rdx = operand, rcx = accumulator,
	// r8d = incoming ACC overflow, r9 = result, r10d = VU lane mask.
	constexpr sptr source_ptr = 0;
	constexpr int operand_ptr = source_ptr + 8;
	constexpr int accumulator_ptr = operand_ptr + 8;
	constexpr int result_ptr = accumulator_ptr + 8;
	constexpr int incoming_acc_overflow = result_ptr + 8;
	constexpr int lane_mask = incoming_acc_overflow + 4;
	constexpr int scalar_mask = lane_mask + 4;
	constexpr int lane_flags = scalar_mask + 12;
	constexpr int stack_size = 72;
	constexpr int lane_overflow = 1;
	constexpr int lane_underflow = 2;

	for (int subtract = 0; subtract < 2; subtract++)
	{
		for (int writes_acc = 0; writes_acc < 2; writes_acc++)
		{
			mVU.softMaddExactVector[subtract][writes_acc] = xGetAlignedCallTarget();
			xSUB(rsp, stack_size);
			xMOV(ptr64[rsp + source_ptr], rax);
			xMOV(ptr64[rsp + operand_ptr], rdx);
			xMOV(ptr64[rsp + accumulator_ptr], rcx);
			xMOV(ptr64[rsp + result_ptr], r9);
			xMOV(ptr32[rsp + incoming_acc_overflow], r8d);
			xMOV(ptr32[rsp + lane_mask], r10d);
			if (writes_acc)
			{
				xMOV(eax, r10d);
				xDEC(eax);
				xTEST(eax, r10d);
				xSETZ(al);
				xMOVZX(eax, al);
				xMOV(ptr32[rsp + scalar_mask], eax);
			}

			for (const size_t offset : {
					 offsetof(VuSoftFmacJitResult, mac_flags),
					 offsetof(VuSoftFmacJitResult, mul_stage_status_flags)})
			{
				xMOV(ptr32[r9 + offset], 0);
			}
			if (writes_acc)
				xMOV(ptr32[r9 + offsetof(VuSoftFmacJitResult, acc_overflow_mask)], 0);
			for (int lane = 0; lane < 4; lane++)
			{
				const u32 lane_bit = 8u >> lane;
				const u32 shift = 3 - lane;
				xTEST(ptr32[rsp + lane_mask], lane_bit);
				xForwardJZ32 inactive_lane;
				xMOV(rax, ptr64[rsp + source_ptr]);
				xMOV(eax, ptr32[rax + lane * 4]);
				xMOV(rdx, ptr64[rsp + operand_ptr]);
				xMOV(edx, ptr32[rdx + lane * 4]);
				xMOV(rcx, ptr64[rsp + accumulator_ptr]);
				xMOV(ecx, ptr32[rcx + lane * 4]);
				xMOV(r8d, ptr32[rsp + incoming_acc_overflow]);
				xAND(r8d, lane_bit);
				xMOV(r9d, subtract);
				xCALL(mVU.softMaddIntegratedLane);
				xMOV(ptr32[rsp + lane_flags], edx);
				xMOV(r9, ptr64[rsp + result_ptr]);
				xOR(ptr32[r9 + offsetof(VuSoftFmacJitResult, mul_stage_status_flags)], ecx);
				xMOV(ptr32[r9 + VU_SOFT_RESULT_VALUE_OFFSET + lane * 4], eax);

				xTEST(eax, 0x80000000);
				xForwardJZ8 result_positive;
				xOR(ptr32[r9 + offsetof(VuSoftFmacJitResult, mac_flags)], 0x0010u << shift);
				result_positive.SetTarget();
				xTEST(ptr32[rsp + lane_flags], lane_underflow);
				xForwardJZ8 result_not_underflow;
				xOR(ptr32[r9 + offsetof(VuSoftFmacJitResult, mac_flags)], 0x0101u << shift);
				xForwardJump8 result_flags_ready;
				result_not_underflow.SetTarget();
				xMOV(ecx, eax);
				xAND(ecx, 0x7fffffff);
				xForwardJNZ8 result_not_zero;
				xOR(ptr32[r9 + offsetof(VuSoftFmacJitResult, mac_flags)], 0x0001u << shift);
				xForwardJump8 result_flags_ready_from_zero;
				result_not_zero.SetTarget();
				xTEST(ptr32[rsp + lane_flags], lane_overflow);
				xForwardJZ8 result_flags_ready_nonexception;
				xOR(ptr32[r9 + offsetof(VuSoftFmacJitResult, mac_flags)], 0x1000u << shift);
				if (writes_acc)
					xOR(ptr32[r9 + offsetof(VuSoftFmacJitResult, acc_overflow_mask)], lane_bit);
				result_flags_ready.SetTarget();
				result_flags_ready_from_zero.SetTarget();
				result_flags_ready_nonexception.SetTarget();
				xForwardJump32 lane_finished;

				inactive_lane.SetTarget();
				if (writes_acc)
				{
					xTEST(ptr32[rsp + scalar_mask], 1);
					xForwardJNZ32 inactive_flags_not_required;
					xMOV(rax, ptr64[rsp + accumulator_ptr]);
					xMOV(eax, ptr32[rax + lane * 4]);
					xTEST(eax, 0x80000000);
					xForwardJZ8 inactive_positive;
					xMOV(r9, ptr64[rsp + result_ptr]);
					xOR(ptr32[r9 + offsetof(VuSoftFmacJitResult, mac_flags)], 0x0010u << shift);
					inactive_positive.SetTarget();
					xAND(eax, 0x7fffffff);
					xForwardJNZ8 inactive_nonzero;
					xMOV(r9, ptr64[rsp + result_ptr]);
					xOR(ptr32[r9 + offsetof(VuSoftFmacJitResult, mac_flags)], 0x0001u << shift);
					xForwardJump8 inactive_flags_ready;
					inactive_nonzero.SetTarget();
					xTEST(ptr32[rsp + incoming_acc_overflow], lane_bit);
					xForwardJZ8 inactive_flags_ready_nonoverflow;
					xMOV(r9, ptr64[rsp + result_ptr]);
					xOR(ptr32[r9 + offsetof(VuSoftFmacJitResult, mac_flags)], 0x1000u << shift);
					inactive_flags_ready.SetTarget();
					inactive_flags_ready_nonoverflow.SetTarget();
					inactive_flags_not_required.SetTarget();
				}
				lane_finished.SetTarget();
			}

			xMOV(r9, ptr64[rsp + result_ptr]);
			mVUemitUpperStatusFromMacFlags(
				ptr32[r9 + offsetof(VuSoftFmacJitResult, mac_flags)],
				ptr32[r9 + offsetof(VuSoftFmacJitResult, status_flags)]);
			xOR(ecx, ptr32[r9 + offsetof(VuSoftFmacJitResult, mul_stage_status_flags)]);
			xMOV(ptr32[r9 + offsetof(VuSoftFmacJitResult, sticky_status_flags)], ecx);
			xADD(rsp, stack_size);
			xRET();
		}
	}
}

static void mVUemitUpperInlineMulExactResult(microVU& mVU, VuUpperFmacSoftDescriptor op,
	const xmm& source, const xmm& operand, const xmm& destination,
	bool allow_fast_normal = true)
{
	constexpr sptr result_offset = 0;
	const int variant = op.OperandVariant();
	const bool use_packed_native = _X_Y_Z_W != 0;
	const bool switch_mxcsr = mVUupperSoftNeedsTruncateMxcsr(mVU);
	const FPControlRegister& vu_fpcr =
		mVU.index == 0 ? EmuConfig.Cpu.VU0FPCR : EmuConfig.Cpu.VU1FPCR;
	const bool flush_to_zero = vu_fpcr.GetFlushToZero();
	const bool native_ps2_zero = flush_to_zero && vu_fpcr.GetDenormalsAreZero();
	const bool switch_native_mxcsr = switch_mxcsr && native_ps2_zero;
	const bool needs_status_flags = mVUupperSoftNeedsStatusValue(mVU);
	const bool needs_mac_flags = mFLAG.doFlag || (op.WritesAcc() && needs_status_flags);
	const bool native_path_available = allow_fast_normal && use_packed_native &&
	                                   (!switch_mxcsr || switch_native_mxcsr);
	const bool emit_stackless_prefix =
		native_path_available && !needs_mac_flags && !needs_status_flags;
	std::optional<xForwardJump32> fast_normal_finished;
	std::optional<xForwardJump32> stackless_mul_finished;
	xmm native_product, native_operand, native_work, native_invalid, native_power;
	if (use_packed_native)
	{
		native_product = mVU.regAlloc->allocReg();
		native_operand = mVU.regAlloc->allocReg();
		native_work = mVU.regAlloc->allocReg();
		native_invalid = mVU.regAlloc->allocReg();
		native_power = mVU.regAlloc->allocReg();
	}
	const auto resultPtr = [](int offset) {
		return ptr32[rsp + result_offset + offset];
	};
	if (emit_stackless_prefix)
	{
		if (switch_native_mxcsr)
			xLDMXCSR(ptr32[&s_vu_soft_truncate_daz_ftz_mxcsr]);
		xMOVAPS(native_product, source);
		mVUemitUpperInlinePrepareOperand(native_operand, operand, variant);
		const bool retain_broadcast_product_abs = native_ps2_zero && variant >= 3;

		// Retain a mask for signed-zero correction and exclude only Booth products
		// whose discarded low region can borrow into retained bit 15. Under
		// DAZ+FTZ, a zero host product is already the exact flag-dead PS2 value,
		// including product underflow, so derive the mask after multiplication.
		if (native_ps2_zero)
		{
			xMUL.PS(native_product, native_operand);
			xPAND(native_power, native_product, ptr128[s_vu_soft_abs]);
			if (retain_broadcast_product_abs)
				xPCMP.EQD(native_invalid, native_power, ptr128[s_vu_soft_zero]);
			else
				xPCMP.EQD(native_power, ptr128[s_vu_soft_zero]);
		}
		else
		{
			xPAND(native_work, native_product, ptr128[s_vu_soft_abs]);
			xPAND(native_invalid, native_operand, ptr128[s_vu_soft_abs]);
			xPMIN.UD(native_power, native_work, native_invalid);
			xPAND(native_power, ptr128[s_vu_soft_exp_field]);
			xPCMP.EQD(native_power, ptr128[s_vu_soft_zero]);
		}
		std::optional<xForwardJump32> stackless_mul_low_half_needs_no_correction;
		if (variant >= 3)
		{
			xMOVD(eax, native_operand);
			xTEST(eax, 0xffff);
			stackless_mul_low_half_needs_no_correction.emplace(Jcc_Zero);
		}
		else
		{
			xPAND(native_work, native_operand, ptr128[s_vu_soft_mantissa]);
			xPCMP.EQD(native_invalid, native_work, ptr128[s_vu_soft_zero]);
		}
		xPMUL.LD(native_work, source, native_operand);
		xPAND(native_work, ptr128[s_vu_soft_mantissa]);
		xPCMP.GTD(native_work, ptr128[s_vu_soft_borrow_limit]);
		if (variant < 3)
			xPOR(native_work, native_invalid);
		xPOR(native_work, retain_broadcast_product_abs ? native_invalid : native_power);
		xMOVMSKPS(eax, native_work);
		xAND(eax, s_vu_soft_lane_mask[_X_Y_Z_W]);
		xCMP(eax, s_vu_soft_lane_mask[_X_Y_Z_W]);
		xForwardJE32 stackless_mul_needs_no_correction;
		xMOV(ecx, s_vu_soft_lane_mask[_X_Y_Z_W]);
		xXOR(ecx, eax); // Active Booth-correction lanes.
		xPAND(native_work, source, ptr128[s_vu_soft_mantissa]);
		xPCMP.EQD(native_work, ptr128[s_vu_soft_zero]);
		xMOVMSKPS(edx, native_work);
		xAND(edx, ecx);
		xCMP(edx, ecx);
		xForwardJNE32 stackless_mul_booth_failed;
		xPXOR(native_invalid, native_invalid);
		if (variant != 0)
		{
			xMOVD(edx, native_operand);
			xAND(edx, 0xffff);
			xMOV64(r11, reinterpret_cast<uptr>(MicroVUSoftFloatTables::first_one_correction_lookup));
			xMOVZX(edx, ptr8[xAddressVoid(r11, rdx, 1)]);
			xMOVDZX(native_invalid, edx);
			xSHUF.PS(native_invalid, native_invalid, 0);
			xSHL(ecx, 4);
			xMOV64(r11, reinterpret_cast<uptr>(s_vu_soft_x86_lane_masks.data()));
			xPAND(native_invalid, ptr128[xAddressVoid(r11, rcx, 1)]);
		}
		else
		{
			for (int lane = 0; lane < 4; lane++)
			{
				if (!(s_vu_soft_lane_mask[_X_Y_Z_W] & (1u << lane)))
					continue;
				xTEST(ecx, 1u << lane);
				xForwardJZ8 stackless_mul_table_lane_ready;
				xPEXTR.D(edx, native_operand, lane);
				xAND(edx, 0xffff);
				xMOV64(r11, reinterpret_cast<uptr>(MicroVUSoftFloatTables::first_one_correction_lookup));
				xMOVZX(edx, ptr8[xAddressVoid(r11, rdx, 1)]);
				xPINSR.D(native_invalid, edx, lane);
				stackless_mul_table_lane_ready.SetTarget();
			}
		}
		if (!native_ps2_zero)
			xMUL.PS(native_product, native_operand);
		xPSUB.D(native_product, native_invalid);
		if (retain_broadcast_product_abs)
			xPAND(native_power, native_product, ptr128[s_vu_soft_abs]);
		xForwardJump32 stackless_mul_product_ready;
		stackless_mul_needs_no_correction.SetTarget();
		if (stackless_mul_low_half_needs_no_correction.has_value())
			stackless_mul_low_half_needs_no_correction->SetTarget();
		if (!native_ps2_zero)
			xMUL.PS(native_product, native_operand);
		stackless_mul_product_ready.SetTarget();
		if (!native_ps2_zero)
		{
			xMOVAPS(native_work, source);
			xPXOR(native_work, native_operand);
			xPAND(native_work, ptr128[s_vu_soft_sign]);
			xPBLEND.VB(native_product, native_product, native_work, native_power);
		}

		if (!retain_broadcast_product_abs)
			xPAND(native_work, native_product, ptr128[s_vu_soft_abs]);
		if (native_ps2_zero)
		{
			xPCMP.GTD(retain_broadcast_product_abs ? native_power : native_work,
				ptr128[s_vu_soft_max_safe]);
		}
		else
		{
			xMOVAPS(native_invalid, ptr128[s_vu_soft_hidden_bit]);
			xPCMP.GTD(native_invalid, native_work);
			xPCMP.GTD(native_work, ptr128[s_vu_soft_max_safe]);
			xPOR(native_invalid, native_work);
			xPANDN(native_power, native_invalid);
		}
		const xmm& result_invalid =
			(native_ps2_zero && !retain_broadcast_product_abs) ? native_work : native_power;
		xMOVMSKPS(eax, result_invalid);
		xAND(eax, s_vu_soft_lane_mask[_X_Y_Z_W]);
		xForwardJNZ32 stackless_mul_result_failed;

		if (switch_native_mxcsr)
			xLDMXCSR(ptr32[mVU.index == 0 ? &EmuConfig.Cpu.VU0FPCR.bitmask : &EmuConfig.Cpu.VU1FPCR.bitmask]);
		mVUemitUpperInlineCommitResult(mVU, destination, native_product);
		if (op.WritesAcc())
			xAND(ptr32[&mVU.regs().accflag], ~static_cast<u32>(_X_Y_Z_W));
		stackless_mul_finished.emplace();

		stackless_mul_booth_failed.SetTarget();
		stackless_mul_result_failed.SetTarget();
		if (switch_native_mxcsr)
			xLDMXCSR(ptr32[mVU.index == 0 ? &EmuConfig.Cpu.VU0FPCR.bitmask : &EmuConfig.Cpu.VU1FPCR.bitmask]);
	}
	if (native_path_available)
	{
		constexpr int fast_source = (sizeof(VuSoftFmacJitResult) + 15) & ~15;
		constexpr int fast_operand = fast_source + 16;
		constexpr int fast_booth_correction = fast_operand + 16;
		constexpr int fast_product_zero = fast_booth_correction + 16;
		constexpr int fast_booth_lane_mask = fast_product_zero + 16;
		constexpr int fast_stack_size = (fast_booth_lane_mask + 4 + 15) & ~15;
		mVUemitUpperSoftStackAlloc(fast_stack_size);
		if (switch_native_mxcsr)
			xLDMXCSR(ptr32[&s_vu_soft_truncate_daz_ftz_mxcsr]);

		xMOVAPS(native_product, source);
		mVUemitUpperInlinePrepareOperand(native_operand, operand, variant);
		const bool operand_is_vf0_one = variant == 6 && _Ft_ == 0;
		const bool operand_is_vf0_zero = variant >= 3 && variant <= 5 && _Ft_ == 0;
		const bool source_is_vf0_one = _Fs_ == 0 && _X_Y_Z_W == 0x1;
		std::optional<xForwardJump32> fast_input_failed;
		std::optional<xForwardJump32> fast_corrected_product_failed;
		std::optional<xForwardJump32> fast_table_product_failed;
		if (operand_is_vf0_zero)
		{
			// VF00.xyz are architectural +0. PS2 multiplication preserves only the
			// source sign in this case, including for denormal and extended inputs.
			xPAND(native_product, ptr128[s_vu_soft_sign]);
		}
		else if (!operand_is_vf0_one)
		{
			xMOVAPS(ptr128[rsp + fast_source], native_product);
			xMOVAPS(ptr128[rsp + fast_operand], native_operand);
			xMOVAPS(native_invalid, native_product);
			xPAND(native_invalid, ptr128[s_vu_soft_abs]);
			xMOVAPS(native_work, native_operand);
			xPAND(native_work, ptr128[s_vu_soft_abs]);
			xMOVAPS(native_power, ptr128[s_vu_soft_hidden_bit]);
			xPCMP.GTD(native_power, native_invalid);
			xMOVAPS(native_product, ptr128[s_vu_soft_hidden_bit]);
			xPCMP.GTD(native_product, native_work);
			xPOR(native_power, native_product);
			xMOVAPS(ptr128[rsp + fast_product_zero], native_power);
			xPCMP.GTD(native_invalid, ptr128[s_vu_soft_max_finite]);
			xPCMP.GTD(native_work, ptr128[s_vu_soft_max_finite]);
			xPOR(native_invalid, native_work);
			xMOVMSKPS(eax, native_invalid);
			xAND(eax, s_vu_soft_lane_mask[_X_Y_Z_W]);
			fast_input_failed.emplace(Jcc_NotZero);
			if (source_is_vf0_one)
			{
				xMOVAPS(native_product, ptr128[rsp + fast_source]);
				xMUL.PS(native_product, ptr128[rsp + fast_operand]);
				mVUemitExtractLane(eax, native_operand, 3);
				xAND(eax, 0xffff);
				xMOV64(r11, reinterpret_cast<uptr>(MicroVUSoftFloatTables::first_one_correction_lookup));
				xMOVZX(ecx, ptr8[xAddressVoid(r11, rax, 1)]);
				xPXOR(native_work, native_work);
				xPINSR.D(native_work, ecx, 3);
				xPSUB.D(native_product, native_work);
			}
			else
			{
				xMOVAPS(native_product, ptr128[rsp + fast_source]);
				xMOVAPS(native_operand, ptr128[rsp + fast_operand]);

				xMOVAPS(native_power, native_operand);
				xPAND(native_power, ptr128[s_vu_soft_mantissa]);
				xPCMP.EQD(native_power, ptr128[s_vu_soft_zero]);
				xMOVAPS(native_product, ptr128[rsp + fast_source]);
				xPMUL.LD(native_product, native_product, ptr128[rsp + fast_operand]);
				xPAND(native_product, ptr128[s_vu_soft_mantissa]);
				xPCMP.GTD(native_product, ptr128[s_vu_soft_borrow_limit]);
				xPOR(native_product, native_power);
				xPOR(native_product, ptr128[rsp + fast_product_zero]);
				xMOVMSKPS(eax, native_product);
				xAND(eax, s_vu_soft_lane_mask[_X_Y_Z_W]);
				xXOR(eax, s_vu_soft_lane_mask[_X_Y_Z_W]);
				xForwardJZ32 fast_product_needs_no_booth_correction;
				xMOV(ptr32[rsp + fast_booth_lane_mask], eax);
				xMOVAPS(native_work, ptr128[rsp + fast_source]);
				xPAND(native_work, ptr128[s_vu_soft_mantissa]);
				xPCMP.EQD(native_work, ptr128[s_vu_soft_zero]);
				xMOVMSKPS(edx, native_work);
				xAND(edx, eax);
				xCMP(edx, eax);
				xForwardJNE32 fast_product_requires_regular_booth;
				xPXOR(native_work, native_work);
				for (int lane = 0; lane < 4; lane++)
				{
					if (!(s_vu_soft_lane_mask[_X_Y_Z_W] & (1u << lane)))
						continue;
					xTEST(eax, 1u << lane);
					xForwardJZ8 fast_table_lane_ready;
					xMOV(ecx, ptr32[rsp + fast_operand + lane * 4]);
					xAND(ecx, 0xffff);
					xMOV64(r11, reinterpret_cast<uptr>(MicroVUSoftFloatTables::first_one_correction_lookup));
					xMOVZX(ecx, ptr8[xAddressVoid(r11, rcx, 1)]);
					xPINSR.D(native_work, ecx, lane);
					fast_table_lane_ready.SetTarget();
				}
				xMOVAPS(native_product, ptr128[rsp + fast_source]);
				xMUL.PS(native_product, ptr128[rsp + fast_operand]);
				xMOVAPS(native_power, native_product);
				xPAND(native_power, ptr128[s_vu_soft_abs]);
				xMOVAPS(native_invalid, ptr128[s_vu_soft_hidden_bit]);
				xPCMP.GTD(native_invalid, native_power);
				xPCMP.GTD(native_power, ptr128[s_vu_soft_max_safe]);
				xPOR(native_invalid, native_power);
				xMOVAPS(native_power, ptr128[rsp + fast_product_zero]);
				xPANDN(native_power, native_invalid);
				xMOVMSKPS(ecx, native_power);
				xAND(ecx, s_vu_soft_lane_mask[_X_Y_Z_W]);
				xTEST(ecx, ecx);
				fast_table_product_failed.emplace(Jcc_NotZero);
				xPSUB.D(native_product, native_work);
				xForwardJump32 fast_table_product_ready;

				fast_product_requires_regular_booth.SetTarget();
				xLEA(rax, ptr[rsp + fast_source]);
				xLEA(rdx, ptr[rsp + fast_operand]);
				xLEA(rcx, ptr[rsp + fast_booth_correction]);
				xMOV(r10d, ptr32[rsp + fast_booth_lane_mask]);
				xCALL(mVU.softMulBoothPacked);
				xMOVAPS(native_product, ptr128[rsp + fast_source]);
				xMOVAPS(native_operand, ptr128[rsp + fast_operand]);
				xMUL.PS(native_product, native_operand);
				xMOVAPS(native_power, native_product);
				xPAND(native_power, ptr128[s_vu_soft_abs]);
				xMOVAPS(native_invalid, ptr128[s_vu_soft_hidden_bit]);
				xPCMP.GTD(native_invalid, native_power);
				xPCMP.GTD(native_power, ptr128[s_vu_soft_max_safe]);
				xPOR(native_invalid, native_power);
				xMOVAPS(native_work, ptr128[rsp + fast_product_zero]);
				xPANDN(native_work, native_invalid);
				xMOVMSKPS(eax, native_work);
				xAND(eax, s_vu_soft_lane_mask[_X_Y_Z_W]);
				fast_corrected_product_failed.emplace(Jcc_NotZero);
				xMOV(eax, ptr32[rsp + fast_booth_lane_mask]);
				xSHL(eax, 4);
				xMOV64(r11, reinterpret_cast<uptr>(s_vu_soft_x86_lane_masks.data()));
				xMOVAPS(native_work, ptr128[xAddressVoid(r11, rax, 1)]);
				xPAND(native_work, ptr128[rsp + fast_booth_correction]);
				xPSUB.D(native_product, native_work);
				xForwardJump32 fast_product_ready_after_booth;

				fast_product_needs_no_booth_correction.SetTarget();
				xMOVAPS(native_product, ptr128[rsp + fast_source]);
				xMUL.PS(native_product, ptr128[rsp + fast_operand]);
				fast_product_ready_after_booth.SetTarget();
				fast_table_product_ready.SetTarget();
			}
			xMOVAPS(native_power, ptr128[rsp + fast_source]);
			xPXOR(native_power, ptr128[rsp + fast_operand]);
			xPAND(native_power, ptr128[s_vu_soft_sign]);
			xMOVAPS(native_invalid, ptr128[rsp + fast_product_zero]);
			if (x86Emitter::use_avx)
			{
				xPBLEND.VB(native_product, native_product, native_power, native_invalid);
			}
			else
			{
				// Avoid SSE4's implicit xmm0 mask without disturbing the
				// allocator-owned registers. native_power and native_work are
				// scratch after the signed-zero selection.
				xPAND(native_power, native_invalid);
				xMOVAPS(native_work, native_invalid);
				xPANDN(native_work, native_product);
				xMOVAPS(native_product, native_power);
				xPOR(native_product, native_work);
			}
		}
		else
		{
			xMOVAPS(native_product, source);
		}
		std::optional<xForwardJump32> fast_result_failed;
		if (!operand_is_vf0_zero)
		{
			xPAND(native_work, native_product, ptr128[s_vu_soft_abs]);
			xPSUB.D(native_work, ptr128[s_vu_soft_hidden_bit]);
			xPXOR(native_work, ptr128[s_vu_soft_sign]);
			xPCMP.GTD(native_work, ptr128[s_vu_soft_safe_range_biased_max]);
			if (!operand_is_vf0_one)
				xPANDN(native_power, native_invalid, native_work);
			xMOVMSKPS(eax, operand_is_vf0_one ? native_work : native_power);
			xAND(eax, s_vu_soft_lane_mask[_X_Y_Z_W]);
			fast_result_failed.emplace(Jcc_NotZero);
		}

		if (switch_native_mxcsr)
			xLDMXCSR(ptr32[mVU.index == 0 ? &EmuConfig.Cpu.VU0FPCR.bitmask : &EmuConfig.Cpu.VU1FPCR.bitmask]);
		mVUemitUpperInlineCommitResult(mVU, destination, native_product);

		if (op.WritesAcc())
			xAND(ptr32[&mVU.regs().accflag], ~static_cast<u32>(_X_Y_Z_W));
		if (needs_mac_flags)
		{
			xMOVMSKPS(eax, native_product);
			xAND(eax, s_vu_soft_lane_mask[_X_Y_Z_W]);
			xMOV64(r11, reinterpret_cast<uptr>(s_vu_soft_lane_mask));
			xMOVZX(eax, ptr8[xAddressVoid(r11, rax, 1)]);
			xSHL(eax, 4);
			if (operand_is_vf0_zero)
			{
				xOR(eax, s_vu_soft_lane_mask[_X_Y_Z_W]);
			}
			else if (!operand_is_vf0_one)
			{
				xMOVMSKPS(edx, native_invalid);
				xAND(edx, s_vu_soft_lane_mask[_X_Y_Z_W]);
				xMOVZX(edx, ptr8[xAddressVoid(r11, rdx, 1)]);
				xOR(eax, edx);
			}
			xMOV(resultPtr(offsetof(VuSoftFmacJitResult, mac_flags)), eax);
			if (needs_status_flags)
			{
				if (!operand_is_vf0_one)
				{
					xXOR(edx, edx);
					xTEST(eax, 0xf);
					xSETNZ(dl);
					xTEST(eax, 0xf0);
					xSETNZ(al);
					xMOVZX(eax, al);
					xSHL(eax, 1);
					xOR(edx, eax);
				}
				else
				{
					xXOR(edx, edx);
					xTEST(eax, eax);
					xSETNZ(dl);
					xSHL(edx, 1);
				}
				xMOV(resultPtr(offsetof(VuSoftFmacJitResult, status_flags)), edx);
			}
		}
		else if (needs_status_flags)
		{
			xMOVMSKPS(eax, native_product);
			xAND(eax, s_vu_soft_lane_mask[_X_Y_Z_W]);
			xXOR(edx, edx);
			xTEST(eax, eax);
			xSETNZ(dl);
			xSHL(edx, 1);
			if (operand_is_vf0_zero)
				xOR(edx, 1);
			else if (!operand_is_vf0_one)
			{
				xMOVMSKPS(eax, native_invalid);
				xAND(eax, s_vu_soft_lane_mask[_X_Y_Z_W]);
				xTEST(eax, eax);
				xSETNZ(al);
				xMOVZX(eax, al);
				xOR(edx, eax);
			}
			xMOV(resultPtr(offsetof(VuSoftFmacJitResult, status_flags)), edx);
		}
		mVUemitSoftFlagWriteback(mVU, result_offset, op);
		mVUemitUpperSoftStackFree(fast_stack_size);
		fast_normal_finished.emplace();

		if (fast_input_failed.has_value())
			fast_input_failed->SetTarget();
		if (fast_result_failed.has_value())
			fast_result_failed->SetTarget();
		if (fast_corrected_product_failed.has_value())
			fast_corrected_product_failed->SetTarget();
		if (fast_table_product_failed.has_value())
			fast_table_product_failed->SetTarget();
		if (switch_native_mxcsr)
			xLDMXCSR(ptr32[mVU.index == 0 ? &EmuConfig.Cpu.VU0FPCR.bitmask : &EmuConfig.Cpu.VU1FPCR.bitmask]);
		mVUemitUpperSoftStackFree(fast_stack_size);
	}
	// Keep uncommon mixed and exceptional domains in a shared out-of-line path.
	{
		constexpr int outlined_source = (sizeof(VuSoftFmacJitResult) + 15) & ~15;
		constexpr int outlined_operand = outlined_source + 16;
		constexpr int outlined_stack_size = outlined_operand + 16;
		mVUemitUpperSoftStackAlloc(outlined_stack_size);
		xMOVAPS(ptr128[rsp + outlined_source], source);
		mVUemitUpperInlinePrepareOperand(native_operand, operand, variant);
		xMOVAPS(ptr128[rsp + outlined_operand], native_operand);
		xLEA(rax, ptr[rsp + outlined_source]);
		xLEA(rdx, ptr[rsp + outlined_operand]);
		xLEA(rcx, ptr[rsp + result_offset]);
		xMOV(r8d, _X_Y_Z_W);
		xCALL(mVU.softMulExactVector);
		xMOVAPS(native_product, resultPtr(offsetof(VuSoftFmacJitResult, value)));
		mVUemitUpperInlineCommitResult(mVU, destination, native_product);
		if (op.WritesAcc())
			mVUemitUpperInlineSoftAccOverflowWriteback(mVU, result_offset);
		mVUemitSoftFlagWriteback(mVU, result_offset, op);
		mVUemitUpperSoftStackFree(outlined_stack_size);
		if (fast_normal_finished.has_value())
			fast_normal_finished->SetTarget();
		if (stackless_mul_finished.has_value())
			stackless_mul_finished->SetTarget();
		mVU.regAlloc->clearNeeded(destination);
		mVU.regAlloc->clearNeeded(native_power);
		mVU.regAlloc->clearNeeded(native_invalid);
		mVU.regAlloc->clearNeeded(native_work);
		mVU.regAlloc->clearNeeded(native_operand);
		mVU.regAlloc->clearNeeded(native_product);
		mVU.regAlloc->clearNeeded(operand);
		mVU.regAlloc->clearNeeded(source);
		return;
	}
}

static void mVUemitUpperInlineMaddExactResult(microVU& mVU, VuUpperFmacSoftDescriptor op,
	const xmm& source, const xmm& operand,
	const xmm& accumulator, const xmm& destination, bool allow_fast_normal = true,
	bool accumulator_nonextended = false, bool source_nonextended = false,
	bool operand_nonextended = false, bool operand_known_normal = false)
{
	constexpr sptr result_offset = 0;
	constexpr int scratch_base = (sizeof(VuSoftFmacJitResult) + 15) & ~15;
	constexpr int incoming_acc_overflow = scratch_base;
	constexpr int lane_flags = incoming_acc_overflow + 4;
	constexpr int source_values = (lane_flags + 16 + 15) & ~15;
	constexpr int operand_values = source_values + 4 * 4;
	constexpr int accumulator_values = operand_values + 4 * 4;
	constexpr int scratch_end = accumulator_values + 4 * 4;
	constexpr int stack_size = (scratch_end + 15) & ~15;
	constexpr int lane_overflow = 1;
	constexpr int lane_underflow = 2;
	const int variant = op.OperandVariant();
	const bool use_packed_native = _X_Y_Z_W != 0;
	const bool use_vector_native_prepare = g_cpu.vectorISA >= ProcessorFeatures::VectorISA::AVX2;
	const bool needs_status_flags = mVUupperSoftNeedsStatusValue(mVU);
	const bool needs_mac_flags = mFLAG.doFlag || (op.WritesAcc() && needs_status_flags);
	const bool needs_result_flags = needs_mac_flags || needs_status_flags;
	const bool use_identity_stackless_madd = variant == 6 && _Ft_ == 0;
	const bool switch_mxcsr = mVUupperSoftNeedsTruncateMxcsr(mVU);
	const FPControlRegister& vu_fpcr =
		mVU.index == 0 ? EmuConfig.Cpu.VU0FPCR : EmuConfig.Cpu.VU1FPCR;
	const bool flush_to_zero = vu_fpcr.GetFlushToZero();
	const bool native_ps2_zero = flush_to_zero && vu_fpcr.GetDenormalsAreZero();
	const bool switch_native_mxcsr = switch_mxcsr && native_ps2_zero;
	const bool native_path_available = allow_fast_normal && use_vector_native_prepare &&
	                                   (!switch_mxcsr || switch_native_mxcsr) && !_XYZW_SS && _X_Y_Z_W != 0;

	const bool emit_stackless_prefix = native_path_available && !needs_result_flags;
	const bool emit_identity_compact = native_path_available && _X_Y_Z_W == 0xf &&
	                                   !emit_stackless_prefix && use_identity_stackless_madd;

	const bool emit_register_fallback = native_path_available && !emit_identity_compact;
	std::optional<xForwardJump32> native_active_flags_ready;
	std::optional<xForwardJump32> outlined_result_ready;
	std::optional<xForwardJump32> compact_common_finished;
	std::optional<xForwardJump32> register_common_finished;
	std::optional<xForwardJump32> stackless_common_finished;
	std::optional<xForwardJump32> zero_product_common_finished;
	xmm native_product, native_operand, native_work, native_invalid, native_power;
	if (use_packed_native)
	{
		native_product = mVU.regAlloc->allocReg();
		native_operand = mVU.regAlloc->allocReg();
		native_work = mVU.regAlloc->allocReg();
		native_invalid = mVU.regAlloc->allocReg();
		native_power = mVU.regAlloc->allocReg();
	}
	const auto resultPtr = [](int offset) {
		return ptr32[rsp + result_offset + offset];
	};

	// Keep the common vector path entirely in allocator-owned registers. Extended
	// exponents, visible Booth corrections, overflow state, and exceptional results
	// fall through to the complete mixed-lane implementation below.
	if (emit_stackless_prefix)
	{
		if (switch_native_mxcsr)
			xLDMXCSR(ptr32[&s_vu_soft_truncate_daz_ftz_mxcsr]);
		xTEST(ptr32[&mVU.regs().accflag], _X_Y_Z_W);
		xForwardJNZ32 stackless_acc_overflow_failed;

		xMOVAPS(native_product, source);
		if (!use_identity_stackless_madd)
		{
			mVUemitUpperInlinePrepareOperand(native_operand, operand, variant);
		}
		if (use_identity_stackless_madd && _X_Y_Z_W != 0xf)
			xBLEND.PS(native_product, ptr128[s_vu_soft_float_one],
				(~s_vu_soft_lane_mask[_X_Y_Z_W]) & 0xf);
		std::optional<xForwardJump32> stackless_input_failed;
		const bool prepare_product_before_booth = native_ps2_zero && !use_identity_stackless_madd;
		if (prepare_product_before_booth)
		{
			// DAZ canonicalizes PS2-zero inputs and FTZ canonicalizes product
			// underflow. Extended inputs produce an exponent-255 host product,
			// which the later add-domain boundary rejects.
			xMUL.PS(native_product, native_operand);
			xPAND(native_power, native_product, ptr128[s_vu_soft_abs]);
			xPCMP.EQD(native_power, ptr128[s_vu_soft_zero]);
		}
		else
		{
			// A small normal operand can hide an extended source in FP modes which
			// do not canonicalize both PS2-zero domains before multiplication.
			xPAND(native_work, native_product, ptr128[s_vu_soft_exp_field]);
			if (use_identity_stackless_madd)
			{
				xPCMP.EQD(native_invalid, native_work, ptr128[s_vu_soft_exp_field]);
				xPTEST(native_invalid, native_invalid);
				stackless_input_failed.emplace(Jcc_NotZero);
				xMOVAPS(native_power, native_work);
			}
			else
			{
				if (!operand_known_normal)
					xPAND(native_invalid, native_operand, ptr128[s_vu_soft_exp_field]);
				if (!source_nonextended || !operand_nonextended)
				{
					if (!source_nonextended && !operand_nonextended)
						xPMAX.UD(native_power, native_work, native_invalid);
					else if (operand_known_normal)
						xMOVAPS(native_power, native_work);
					else
						xMOVAPS(native_power, source_nonextended ? native_invalid : native_work);
					xPCMP.EQD(native_power, ptr128[s_vu_soft_exp_field]);
					xPTEST(native_power, native_power);
					stackless_input_failed.emplace(Jcc_NotZero);
				}
				if (operand_known_normal)
					xMOVAPS(native_power, native_work);
				else
					xPMIN.UD(native_power, native_work, native_invalid);
			}
			xPCMP.EQD(native_power, ptr128[s_vu_soft_zero]);
		}
		std::optional<xForwardJump32> stackless_product_failed;
		std::optional<xForwardJump32> stackless_booth_product_failed;
		std::optional<xForwardJump32> stackless_acc_input_failed;
		std::optional<xForwardJump32> stackless_difference_failed;
		std::optional<xForwardJump32> stackless_result_failed;
		if (!use_identity_stackless_madd)
		{
			// Reject only products for which the PS2 Booth correction can reach a
			// retained bit. The complete generated path owns those uncommon lanes.
			std::optional<xForwardJump32> stackless_low_half_needs_no_booth;
			if (variant >= 3)
			{
				// Booth's discarded tree is zero for every lane when the shared
				// second mantissa has no low-half bits.
				xMOVD(eax, native_operand);
				xTEST(eax, 0xffff);
				stackless_low_half_needs_no_booth.emplace(Jcc_Zero);
			}
			else
			{
				xPAND(native_work, native_operand, ptr128[s_vu_soft_mantissa]);
				xPCMP.EQD(native_invalid, native_work, ptr128[s_vu_soft_zero]);
			}
			xPMUL.LD(native_work, source, native_operand);
			xPAND(native_work, ptr128[s_vu_soft_mantissa]);
			xPCMP.GTD(native_work, ptr128[s_vu_soft_borrow_limit]);
			if (variant < 3)
				xPOR(native_work, native_invalid);
			xPOR(native_work, native_power);
			xMOVMSKPS(eax, native_work);
			xXOR(eax, 0xf);
			xAND(eax, s_vu_soft_lane_mask[_X_Y_Z_W]);
			stackless_booth_product_failed.emplace(Jcc_NotZero);
			if (stackless_low_half_needs_no_booth.has_value())
				stackless_low_half_needs_no_booth->SetTarget();
			if (!prepare_product_before_booth)
				xMUL.PS(native_product, native_operand);
			if (!flush_to_zero)
			{
				xPAND(native_work, native_product, ptr128[s_vu_soft_abs]);
				xMOVAPS(native_invalid, ptr128[s_vu_soft_hidden_bit]);
				xPCMP.GTD(native_invalid, native_work);
				xPANDN(native_invalid, native_power, native_invalid);
				xPTEST(native_invalid, native_invalid);
				stackless_product_failed.emplace(Jcc_NotZero);
			}
		}
		if (op.IsKind(VuUpperFmacSoftKind::Msub))
			xPXOR(native_product, ptr128[s_vu_soft_sign]);

		xMOVAPS(native_operand, accumulator);
		if (use_identity_stackless_madd && _X_Y_Z_W != 0xf)
			xBLEND.PS(native_operand, ptr128[s_vu_soft_float_one],
				(~s_vu_soft_lane_mask[_X_Y_Z_W]) & 0xf);
		xPAND(native_work, native_operand, ptr128[s_vu_soft_exp_field]);
		if (!accumulator_nonextended)
		{
			xPCMP.EQD(native_invalid, native_work, ptr128[s_vu_soft_exp_field]);
			xPTEST(native_invalid, native_invalid);
			stackless_acc_input_failed.emplace(Jcc_NotZero);
		}
		xPAND(native_invalid, native_product, ptr128[s_vu_soft_exp_field]);
		// Product-zero lanes preserve ACC and therefore need no exponent-gap
		// fallback. Giving them ACC's exponent makes their alignment shift zero;
		// native_power is still the proven PS2 product-zero mask here.
		xPBLEND.VB(native_invalid, native_invalid, native_work, native_power);
		if (flush_to_zero)
		{
			// With both terms at exponent 253 or below, their sum cannot reach
			// exponent 255. Prove that before the add so its result needs no
			// dependent upper-boundary classification.
			xPMAX.UD(native_power, native_work, native_invalid);
			xPCMP.GTD(native_power, ptr128[s_vu_soft_exp_field_253]);
			xPTEST(native_power, native_power);
			stackless_result_failed.emplace(Jcc_NotZero);
		}
		// The complete path owns nonzero-product exponent gaps above 24 and
		// their exact mixed-lane rounding behavior.
		xPSUB.D(native_work, native_invalid);
		xPSRA.D(native_work, 23);
		xPABS.D(native_power, native_work);
		xPCMP.GTD(native_invalid, native_power, ptr128[s_vu_soft_exp_24]);
		xPTEST(native_invalid, native_invalid);
		stackless_difference_failed.emplace(Jcc_NotZero);
		xPSUB.D(native_power, ptr128[s_vu_soft_one]);
		xPMAX.SD(native_power, ptr128[s_vu_soft_zero]);
		xPCMP.GTD(native_invalid, native_work, ptr128[s_vu_soft_zero]);
		xPBLEND.VB(native_work, native_operand, native_product, native_invalid);
		xPBLEND.VB(native_product, native_product, native_operand, native_invalid);
		xVPSRLVD(native_work, native_work, native_power);
		xVPSLLVD(native_work, native_work, native_power);
		xADD.PS(native_product, native_work);
		if (use_identity_stackless_madd)
		{
			xPAND(native_work, source, ptr128[s_vu_soft_abs]);
			xMOVAPS(native_invalid, ptr128[s_vu_soft_hidden_bit]);
			xPCMP.GTD(native_invalid, native_work);
			xPBLEND.VB(native_product, native_product, accumulator, native_invalid);
		}

		if (!flush_to_zero)
		{
			// A computed denormal is not representable as a PS2 normal result.
			// Exact zero and normal finite results can commit directly.
			xPAND(native_work, native_product, ptr128[s_vu_soft_abs]);
			xPCMP.EQD(native_invalid, native_work, ptr128[s_vu_soft_zero]);
			xPSUB.D(native_work, ptr128[s_vu_soft_hidden_bit]);
			xPXOR(native_work, ptr128[s_vu_soft_sign]);
			xPCMP.GTD(native_work, ptr128[s_vu_soft_safe_range_biased_max]);
			xPANDN(native_power, native_invalid, native_work);
			xPTEST(native_power, native_power);
			stackless_result_failed.emplace(Jcc_NotZero);
		}
		if (switch_native_mxcsr)
			xLDMXCSR(ptr32[mVU.index == 0 ? &EmuConfig.Cpu.VU0FPCR.bitmask : &EmuConfig.Cpu.VU1FPCR.bitmask]);
		mVUemitUpperInlineCommitResult(mVU, destination, native_product);
		// The entry guard proved the active ACC overflow bits clear, and the
		// accepted result cannot set them. Inactive masked bits remain untouched.
		stackless_common_finished.emplace();

		stackless_acc_overflow_failed.SetTarget();
		if (stackless_input_failed.has_value())
			stackless_input_failed->SetTarget();
		if (stackless_product_failed.has_value())
			stackless_product_failed->SetTarget();
		if (stackless_booth_product_failed.has_value())
			stackless_booth_product_failed->SetTarget();
		if (stackless_acc_input_failed.has_value())
			stackless_acc_input_failed->SetTarget();
		if (stackless_difference_failed.has_value())
			stackless_difference_failed->SetTarget();
		if (stackless_result_failed.has_value())
			stackless_result_failed->SetTarget();
		if (switch_native_mxcsr)
			xLDMXCSR(ptr32[mVU.index == 0 ? &EmuConfig.Cpu.VU0FPCR.bitmask : &EmuConfig.Cpu.VU1FPCR.bitmask]);
	}
	// These tiers allocate different temporary registers. Emitting both for one
	// instruction would let a stackless success skip compact-tier spill code which
	// the compile-time allocator has already incorporated into its state.
	if (emit_identity_compact)
	{
		if (switch_native_mxcsr)
			xLDMXCSR(ptr32[&s_vu_soft_truncate_daz_ftz_mxcsr]);
		const xmm& compact_domain = mVU.regAlloc->allocReg();
		const xmm& compact_product_zero = mVU.regAlloc->allocReg();

		xTEST(ptr32[&mVU.regs().accflag], 0xf);
		xForwardJNZ32 compact_acc_overflow_failed;
		xMOVAPS(native_product, source);
		xPXOR(compact_product_zero, compact_product_zero);
		xPAND(native_work, native_product, ptr128[s_vu_soft_abs]);
		xPSUB.D(native_work, ptr128[s_vu_soft_hidden_bit]);
		xPXOR(native_work, ptr128[s_vu_soft_sign]);
		xPCMP.GTD(native_work, ptr128[s_vu_soft_safe_range_biased_max]);
		xMOVAPS(compact_domain, native_work);

		if (needs_status_flags)
		{
			xMOVMSKPS(eax, native_product);
			xXOR(edx, edx);
			xTEST(eax, eax);
			xSETNZ(dl);
			xSHL(edx, 1);
			xMOV(r9d, edx);
			xMOVMSKPS(eax, compact_product_zero);
			xXOR(edx, edx);
			xTEST(eax, eax);
			xSETNZ(dl);
			xOR(r9d, edx);
		}
		if (op.IsKind(VuUpperFmacSoftKind::Msub))
			xPXOR(native_product, ptr128[s_vu_soft_sign]);

		xMOVAPS(native_operand, accumulator);
		xPAND(native_work, native_operand, ptr128[s_vu_soft_abs]);
		xPSUB.D(native_work, ptr128[s_vu_soft_hidden_bit]);
		xPXOR(native_work, ptr128[s_vu_soft_sign]);
		xPCMP.GTD(native_work, ptr128[s_vu_soft_finite_range_biased_max]);
		xPOR(compact_domain, native_work);
		// Apply the PS2's exponent-distance truncation to the smaller term.
		xPSRL.D(native_work, native_operand, 23);
		xPAND(native_work, ptr128[s_vu_soft_exp_mask]);
		xPSRL.D(native_power, native_product, 23);
		xPAND(native_power, ptr128[s_vu_soft_exp_mask]);
		xPSUB.D(native_work, native_power);
		xPABS.D(native_invalid, native_work);
		xPSUB.D(native_invalid, ptr128[s_vu_soft_one]);
		xPMAX.SD(native_invalid, ptr128[s_vu_soft_zero]);
		xPCMP.GTD(native_power, native_work, ptr128[s_vu_soft_zero]);
		xPBLEND.VB(native_work, native_operand, native_product, native_power);
		xPBLEND.VB(native_product, native_product, native_operand, native_power);
		xVPSRLVD(native_work, native_work, native_invalid);
		xVPSLLVD(native_work, native_work, native_invalid);
		xADD.PS(native_product, native_work);
		xPBLEND.VB(native_product, native_product, native_operand, compact_product_zero);

		xPAND(native_work, native_product, ptr128[s_vu_soft_abs]);
		xPSUB.D(native_work, ptr128[s_vu_soft_hidden_bit]);
		xPXOR(native_work, ptr128[s_vu_soft_sign]);
		xPCMP.GTD(native_work, ptr128[s_vu_soft_safe_range_biased_max]);
		xPOR(native_work, compact_domain);
		xPTEST(native_work, native_work);
		xForwardJNZ32 compact_domain_failed;

		xMOVAPS(destination, native_product);
		if (needs_mac_flags)
		{
			xMOVMSKPS(ecx, native_product);
			xMOV64(r11, reinterpret_cast<uptr>(s_vu_soft_lane_mask));
			xMOVZX(ecx, ptr8[xAddressVoid(r11, rcx, 1)]);
			xSHL(ecx, 4);
			if (mFLAG.doFlag)
				mVUallocMFLAGb(mVU, ecx, mFLAG.write);
		}
		if (needs_status_flags)
		{
			xMOVMSKPS(ecx, native_product);
			xXOR(edx, edx);
			xTEST(ecx, ecx);
			xSETNZ(dl);
			xSHL(edx, 1);
			xOR(r9d, edx);
			mVUemitSoftSFlagWriteback(mVU, edx, r9d);
		}
		if (op.WritesAcc())
			xMOV(ptr32[&mVU.regs().accflag], 0);
		if (switch_native_mxcsr)
			xLDMXCSR(ptr32[mVU.index == 0 ? &EmuConfig.Cpu.VU0FPCR.bitmask : &EmuConfig.Cpu.VU1FPCR.bitmask]);
		compact_common_finished.emplace();

		compact_acc_overflow_failed.SetTarget();
		compact_domain_failed.SetTarget();
		if (switch_native_mxcsr)
			xLDMXCSR(ptr32[mVU.index == 0 ? &EmuConfig.Cpu.VU0FPCR.bitmask : &EmuConfig.Cpu.VU1FPCR.bitmask]);
		mVU.regAlloc->clearNeeded(compact_product_zero);
		mVU.regAlloc->clearNeeded(compact_domain);
	}
	else if (emit_register_fallback)
	{
		constexpr int register_input_zero = (sizeof(VuSoftFmacJitResult) + 15) & ~15;
		constexpr int register_product_zero = register_input_zero;
		constexpr int register_acc_zero = register_product_zero + 16;
		constexpr int register_both_zero = register_acc_zero + 16;
		constexpr int register_cancellation = register_both_zero + 16;
		constexpr int register_source = register_cancellation + 16;
		constexpr int register_operand = register_source + 16;
		constexpr int register_booth_correction = register_operand + 16;
		constexpr int register_booth_lane_mask = register_booth_correction + 16;
		constexpr int register_stack_size = (register_booth_lane_mask + 4 + 15) & ~15;
		mVUemitUpperSoftStackAlloc(register_stack_size);
		if (switch_native_mxcsr)
			xLDMXCSR(ptr32[&s_vu_soft_truncate_daz_ftz_mxcsr]);

		xTEST(ptr32[&mVU.regs().accflag], 0xf);
		xForwardJNZ32 register_acc_overflow_failed;
		std::optional<xForwardJump32> register_input_failed;
		std::optional<xForwardJump32> register_corrected_product_failed;
		xMOVAPS(native_product, source);
		mVUemitUpperInlinePrepareOperand(native_operand, operand, variant);
		if (_X_Y_Z_W != 0xf)
		{
			xBLEND.PS(native_product, ptr128[s_vu_soft_float_one],
				(~s_vu_soft_lane_mask[_X_Y_Z_W]) & 0xf);
			xBLEND.PS(native_operand, ptr128[s_vu_soft_float_one],
				(~s_vu_soft_lane_mask[_X_Y_Z_W]) & 0xf);
		}
		const bool operand_is_vf0_one = variant == 6 && _Ft_ == 0;
		if (_X_Y_Z_W != 0xf)
		{
			xMOVAPS(ptr128[rsp + register_source], native_product);
			xMOVAPS(ptr128[rsp + register_operand], native_operand);
		}
		if (!operand_is_vf0_one)
		{
			// Classify both inputs by absolute bit range. Values below the hidden bit are
			// PS2 zeros; values above the largest finite encoding require exact fallback.
			xPAND(native_power, native_product, ptr128[s_vu_soft_abs]);
			xPAND(native_work, native_operand, ptr128[s_vu_soft_abs]);
			xPMIN.UD(native_power, native_work);
			xMOVAPS(native_invalid, ptr128[s_vu_soft_hidden_bit]);
			xPCMP.GTD(native_invalid, native_power);
			xMOVAPS(ptr128[rsp + register_input_zero], native_invalid);
			// The host product differs only when the PS2 Booth tree borrows into retained
			// bit 15. Prove that no active normal lane can observe that correction.
			xPAND(native_power, native_operand, ptr128[s_vu_soft_mantissa]);
			xPCMP.EQD(native_power, ptr128[s_vu_soft_zero]);
			if (_X_Y_Z_W == 0xf)
				xPMUL.LD(native_product, source, native_operand);
			else
			{
				xMOVAPS(native_product, ptr128[rsp + register_source]);
				xPMUL.LD(native_product, native_product, ptr128[rsp + register_operand]);
			}
			xPAND(native_product, ptr128[s_vu_soft_mantissa]);
			xPCMP.GTD(native_product, ptr128[s_vu_soft_borrow_limit]);
			xPOR(native_product, native_power);
			xPOR(native_product, native_invalid);
			xMOVMSKPS(eax, native_product);
			xXOR(eax, 0xf);
			xForwardJZ32 register_product_needs_no_booth_correction;
			xMOV(ptr32[rsp + register_booth_lane_mask], eax);
			if (_X_Y_Z_W == 0xf)
			{
				xMOVAPS(ptr128[rsp + register_source], source);
				xMOVAPS(ptr128[rsp + register_operand], native_operand);
			}
			std::optional<xForwardJump32> register_regular_booth;
			std::optional<xForwardJump32> register_booth_correction_ready;
			if (_X_Y_Z_W == 0xf && !needs_result_flags)
			{
				xMOVAPS(native_work, ptr128[rsp + register_source]);
				xPAND(native_work, ptr128[s_vu_soft_mantissa]);
				xPCMP.EQD(native_work, ptr128[s_vu_soft_zero]);
				xMOVMSKPS(edx, native_work);
				xAND(edx, eax);
				xCMP(edx, eax);
				register_regular_booth.emplace(Jcc_NotEqual);
				xPXOR(native_work, native_work);
				for (int lane = 0; lane < 4; lane++)
				{
					xTEST(eax, 1u << lane);
					xForwardJZ8 register_table_lane_ready;
					xMOV(ecx, ptr32[rsp + register_operand + lane * 4]);
					xAND(ecx, 0xffff);
					xMOV64(r11, reinterpret_cast<uptr>(MicroVUSoftFloatTables::first_one_correction_lookup));
					xMOVZX(ecx, ptr8[xAddressVoid(r11, rcx, 1)]);
					xPINSR.D(native_work, ecx, lane);
					register_table_lane_ready.SetTarget();
				}
				xMOVAPS(ptr128[rsp + register_booth_correction], native_work);
				register_booth_correction_ready.emplace();
				register_regular_booth->SetTarget();
			}
			xLEA(rax, ptr[rsp + register_source]);
			xLEA(rdx, ptr[rsp + register_operand]);
			xLEA(rcx, ptr[rsp + register_booth_correction]);
			xMOV(r10d, ptr32[rsp + register_booth_lane_mask]);
			xCALL(mVU.softMulBoothPacked);
			if (register_booth_correction_ready.has_value())
				register_booth_correction_ready->SetTarget();
			xMOVAPS(native_product, ptr128[rsp + register_source]);
			xMOVAPS(native_operand, ptr128[rsp + register_operand]);
			xMUL.PS(native_product, native_operand);
			xPAND(native_power, native_product, ptr128[s_vu_soft_abs]);
			xMOVAPS(native_invalid, ptr128[s_vu_soft_hidden_bit]);
			xPCMP.GTD(native_invalid, native_power);
			xPCMP.GTD(native_power, ptr128[s_vu_soft_max_safe]);
			xPOR(native_invalid, native_power);
			xMOVAPS(native_work, ptr128[rsp + register_input_zero]);
			xPANDN(native_work, native_invalid);
			xMOVMSKPS(eax, native_work);
			xTEST(eax, eax);
			register_corrected_product_failed.emplace(Jcc_NotZero);
			xMOV(eax, ptr32[rsp + register_booth_lane_mask]);
			xSHL(eax, 4);
			xMOV64(r11, reinterpret_cast<uptr>(s_vu_soft_x86_lane_masks.data()));
			xMOVAPS(native_work, ptr128[xAddressVoid(r11, rax, 1)]);
			xPAND(native_work, ptr128[rsp + register_booth_correction]);
			xPSUB.D(native_product, native_work);
			xForwardJump32 register_product_ready_after_booth;

			register_product_needs_no_booth_correction.SetTarget();
			if (_X_Y_Z_W == 0xf)
				xMUL.PS(native_product, source, native_operand);
			else
			{
				xMOVAPS(native_product, ptr128[rsp + register_source]);
				xMUL.PS(native_product, ptr128[rsp + register_operand]);
			}
			register_product_ready_after_booth.SetTarget();
		}
		else
		{
			xMOVAPS(native_product, source);
			xMOVAPS(native_invalid, ptr128[s_vu_soft_zero]);
			xMOVAPS(ptr128[rsp + register_input_zero], native_invalid);
		}
		xPAND(native_work, native_product, ptr128[s_vu_soft_abs]);
		xPSUB.D(native_work, ptr128[s_vu_soft_hidden_bit]);
		xPXOR(native_work, ptr128[s_vu_soft_sign]);
		xPCMP.GTD(native_work, ptr128[s_vu_soft_safe_range_biased_max]);
		xMOVAPS(native_invalid, ptr128[rsp + register_input_zero]);
		xPANDN(native_power, native_invalid, native_work);
		xMOVMSKPS(eax, native_power);
		xTEST(eax, eax);
		xForwardJNZ32 register_product_failed;
		if (needs_status_flags)
		{
			xMOVMSKPS(eax, native_product);
			xAND(eax, s_vu_soft_lane_mask[_X_Y_Z_W]);
			xXOR(edx, edx);
			xTEST(eax, eax);
			xSETNZ(dl);
			xSHL(edx, 1);
			xMOVMSKPS(eax, native_invalid);
			xTEST(eax, eax);
			xSETNZ(al);
			xMOVZX(eax, al);
			xOR(edx, eax);
			xMOV(resultPtr(offsetof(VuSoftFmacJitResult, mul_stage_status_flags)), edx);
		}
		if (op.IsKind(VuUpperFmacSoftKind::Msub))
			xPXOR(native_product, ptr128[s_vu_soft_sign]);

		xMOVAPS(native_operand, accumulator);
		if (_X_Y_Z_W != 0xf)
			xBLEND.PS(native_operand, ptr128[s_vu_soft_float_one],
				(~s_vu_soft_lane_mask[_X_Y_Z_W]) & 0xf);
		xPAND(native_work, native_operand, ptr128[s_vu_soft_abs]);
		xPCMP.GTD(native_invalid, native_work, ptr128[s_vu_soft_max_finite]);
		xMOVMSKPS(eax, native_invalid);
		xTEST(eax, eax);
		xForwardJNZ32 register_acc_input_failed;
		xMOVAPS(native_power, ptr128[s_vu_soft_hidden_bit]);
		xPCMP.GTD(native_power, native_work);
		xMOVAPS(ptr128[rsp + register_acc_zero], native_power);
		xPAND(native_power, ptr128[rsp + register_product_zero]);
		xMOVAPS(ptr128[rsp + register_both_zero], native_power);

		// Pretruncate the smaller normal operand exactly as PS2Float::Add/Sub does.
		xPSRL.D(native_work, native_operand, 23);
		xPAND(native_work, ptr128[s_vu_soft_exp_mask]);
		xPSRL.D(native_power, native_product, 23);
		xPAND(native_power, ptr128[s_vu_soft_exp_mask]);
		xPSUB.D(native_work, native_power);
		xPABS.D(native_invalid, native_work);
		xPCMP.GTD(native_power, native_invalid, ptr128[s_vu_soft_exp_24]);
		// A variable shift count of 32 discards the smaller term, matching
		// PS2Float::Add/Sub when the exponent gap exceeds 24. Use the same
		// count-forcing form as the stackless add path so no lane-mask spill is
		// needed here.
		xPBLEND.VB(native_invalid, native_invalid, ptr128[s_vu_soft_exp_33], native_power);
		xPSUB.D(native_invalid, ptr128[s_vu_soft_one]);
		xPMAX.SD(native_invalid, ptr128[s_vu_soft_zero]);

		// Addition is commutative after MSUB has negated the product. Select
		// the smaller term once, apply the PS2 alignment mask once, and add it
		// to the unmodified larger term.
		xPCMP.GTD(native_power, native_work, ptr128[s_vu_soft_zero]);
		xPBLEND.VB(native_work, native_operand, native_product, native_power);
		xPBLEND.VB(native_product, native_product, native_operand, native_power);
		xVPSRLVD(native_work, native_work, native_invalid);
		xVPSLLVD(native_invalid, native_work, native_invalid);
		xPXOR(native_power, native_invalid, ptr128[s_vu_soft_sign]);
		xPCMP.EQD(native_power, native_power, native_product);
		xMOVAPS(ptr128[rsp + register_cancellation], native_power);
		xADD.PS(native_product, native_invalid);

		// A zero ACC naturally leaves the corrected product unchanged after alignment;
		// a zero product selects the unchanged ACC explicitly.
		xMOVAPS(native_power, ptr128[rsp + register_product_zero]);
		xPBLEND.VB(native_product, native_product, native_operand, native_power);

		// When both terms are PS2 zeros, Add/Sub chooses a signed zero from both
		// effective signs. Handle those lanes explicitly instead of rejecting the
		// complete vector. The ACC-zero mask remains available to status timing.
		xMOVAPS(native_power, ptr128[rsp + register_both_zero]);
		xMOVMSKPS(eax, native_power);
		xTEST(eax, eax);
		xForwardJZ32 register_no_both_zero_adjustment;
		xMOVAPS(native_invalid, source);
		xMOVAPS(native_work, operand);
		if (variant != 0)
			xPSHUF.D(native_work, native_work, variant >= 3 ? (variant - 3) * 0x55 : 0);
		xPXOR(native_invalid, native_work);
		if (op.IsKind(VuUpperFmacSoftKind::Msub))
			xPXOR(native_invalid, ptr128[s_vu_soft_sign]);
		xPAND(native_invalid, native_operand);
		xPAND(native_invalid, ptr128[s_vu_soft_sign]);
		xPBLEND.VB(native_product, native_product, native_invalid, native_power);
		register_no_both_zero_adjustment.SetTarget();
		xMOVAPS(native_power, ptr128[rsp + register_both_zero]);
		xMOVAPS(native_invalid, ptr128[rsp + register_cancellation]);
		xPANDN(native_power, native_invalid);
		xMOVAPS(native_invalid, ptr128[s_vu_soft_zero]);
		xPBLEND.VB(native_product, native_product, native_invalid, native_power);
		xPOR(native_power, ptr128[rsp + register_both_zero]);
		xMOVAPS(ptr128[rsp + register_both_zero], native_power);
		xPAND(native_work, native_product, ptr128[s_vu_soft_abs]);
		xPSUB.D(native_work, ptr128[s_vu_soft_hidden_bit]);
		xPXOR(native_work, ptr128[s_vu_soft_sign]);
		// Round-toward-zero saturates a host overflow at max finite. Exclude that
		// boundary so PS2 extended-exponent results use the exact fallback.
		xPCMP.GTD(native_work, ptr128[s_vu_soft_safe_range_biased_max]);
		xMOVAPS(native_invalid, ptr128[rsp + register_both_zero]);
		xPANDN(native_power, native_invalid, native_work);
		xMOVMSKPS(eax, native_power);
		xTEST(eax, eax);
		xForwardJNZ32 register_result_failed;
		mVUemitUpperInlineCommitResult(mVU, destination, native_product);
		const bool register_flags_cover_acc = op.WritesAcc();
		const xmm& register_flag_value = register_flags_cover_acc ? destination : native_product;
		const u32 register_flag_lane_mask =
			register_flags_cover_acc ? 0xf : s_vu_soft_lane_mask[_X_Y_Z_W];
		if (register_flags_cover_acc)
		{
			// ACC flags describe the merged vector, including lanes which this
			// masked instruction did not update.
			xPAND(native_invalid, destination, ptr128[s_vu_soft_abs]);
			xPCMP.EQD(native_invalid, ptr128[s_vu_soft_zero]);
		}
		if (needs_mac_flags)
		{
			xMOVMSKPS(ecx, register_flag_value);
			xAND(ecx, register_flag_lane_mask);
			xMOV64(r11, reinterpret_cast<uptr>(s_vu_soft_lane_mask));
			xMOVZX(ecx, ptr8[xAddressVoid(r11, rcx, 1)]);
			xSHL(ecx, 4);
			xMOVMSKPS(eax, native_invalid);
			xAND(eax, register_flag_lane_mask);
			xMOVZX(eax, ptr8[xAddressVoid(r11, rax, 1)]);
			xOR(ecx, eax);
			xMOV(resultPtr(offsetof(VuSoftFmacJitResult, mac_flags)), ecx);
			if (needs_status_flags)
			{
				xXOR(eax, eax);
				xTEST(ecx, 0xf);
				xSETNZ(al);
				xXOR(edx, edx);
				xTEST(ecx, 0xf0);
				xSETNZ(dl);
				xSHL(edx, 1);
				xOR(eax, edx);
				xMOV(resultPtr(offsetof(VuSoftFmacJitResult, status_flags)), eax);
				xOR(eax, resultPtr(offsetof(VuSoftFmacJitResult, mul_stage_status_flags)));
				xMOV(resultPtr(offsetof(VuSoftFmacJitResult, sticky_status_flags)), eax);
			}
		}
		else if (needs_status_flags)
		{
			xMOVMSKPS(ecx, register_flag_value);
			xAND(ecx, register_flag_lane_mask);
			xXOR(eax, eax);
			xTEST(ecx, ecx);
			xSETNZ(al);
			xSHL(eax, 1);
			xMOVMSKPS(ecx, native_invalid);
			xAND(ecx, register_flag_lane_mask);
			xXOR(edx, edx);
			xTEST(ecx, ecx);
			xSETNZ(dl);
			xOR(eax, edx);
			xMOV(resultPtr(offsetof(VuSoftFmacJitResult, status_flags)), eax);
			xOR(eax, resultPtr(offsetof(VuSoftFmacJitResult, mul_stage_status_flags)));
			xMOV(resultPtr(offsetof(VuSoftFmacJitResult, sticky_status_flags)), eax);
		}
		if (op.WritesAcc())
			xAND(ptr32[&mVU.regs().accflag], ~static_cast<u32>(_X_Y_Z_W));
		mVUemitSoftFlagWriteback(mVU, result_offset, op);
		if (switch_native_mxcsr)
			xLDMXCSR(ptr32[mVU.index == 0 ? &EmuConfig.Cpu.VU0FPCR.bitmask : &EmuConfig.Cpu.VU1FPCR.bitmask]);
		mVUemitUpperSoftStackFree(register_stack_size);
		register_common_finished.emplace();

		register_acc_overflow_failed.SetTarget();
		if (register_input_failed.has_value())
			register_input_failed->SetTarget();
		register_product_failed.SetTarget();
		if (register_corrected_product_failed.has_value())
			register_corrected_product_failed->SetTarget();
		register_acc_input_failed.SetTarget();
		register_result_failed.SetTarget();
		if (switch_native_mxcsr)
			xLDMXCSR(ptr32[mVU.index == 0 ? &EmuConfig.Cpu.VU0FPCR.bitmask : &EmuConfig.Cpu.VU1FPCR.bitmask]);
		mVUemitUpperSoftStackFree(register_stack_size);
	}
	// Every vector direct miss uses one shared generated exact kernel. Scalar instructions
	// continue through the inline exact loop below.
	if (!_XYZW_SS)
	{
		constexpr int exact_vector_source = (sizeof(VuSoftFmacJitResult) + 15) & ~15;
		constexpr int exact_vector_operand = exact_vector_source + 16;
		constexpr int exact_vector_accumulator = exact_vector_operand + 16;
		constexpr int exact_vector_stack_size = exact_vector_accumulator + 16;
		mVUemitUpperSoftStackAlloc(exact_vector_stack_size);
		xMOVAPS(ptr128[rsp + exact_vector_source], source);
		mVUemitUpperInlinePrepareOperand(native_operand, operand, variant);
		xMOVAPS(ptr128[rsp + exact_vector_operand], native_operand);
		xMOVAPS(ptr128[rsp + exact_vector_accumulator], accumulator);
		xLEA(rax, ptr[rsp + exact_vector_source]);
		xLEA(rdx, ptr[rsp + exact_vector_operand]);
		xLEA(rcx, ptr[rsp + exact_vector_accumulator]);
		xMOV(r8d, ptr32[&mVU.regs().accflag]);
		xLEA(r9, ptr[rsp + result_offset]);
		xMOV(r10d, _X_Y_Z_W);
		xCALL(mVU.softMaddExactVector[op.IsKind(VuUpperFmacSoftKind::Msub) ? 1 : 0]
									 [op.WritesAcc() ? 1 : 0]);

		xMOVAPS(native_product, resultPtr(offsetof(VuSoftFmacJitResult, value)));
		mVUemitUpperInlineCommitResult(mVU, destination, native_product);
		if (op.WritesAcc())
			mVUemitUpperInlineSoftAccOverflowWriteback(mVU, result_offset);
		mVUemitSoftFlagWriteback(mVU, result_offset, op);
		mVUemitUpperSoftStackFree(exact_vector_stack_size);
		if (register_common_finished.has_value())
			register_common_finished->SetTarget();
		if (compact_common_finished.has_value())
			compact_common_finished->SetTarget();
		if (stackless_common_finished.has_value())
			stackless_common_finished->SetTarget();
		if (zero_product_common_finished.has_value())
			zero_product_common_finished->SetTarget();
		mVU.regAlloc->clearNeeded(destination);
		if (destination.Id != accumulator.Id)
			mVU.regAlloc->clearNeeded(accumulator);
		if (use_packed_native)
		{
			mVU.regAlloc->clearNeeded(native_power);
			mVU.regAlloc->clearNeeded(native_invalid);
			mVU.regAlloc->clearNeeded(native_work);
			mVU.regAlloc->clearNeeded(native_operand);
			mVU.regAlloc->clearNeeded(native_product);
		}
		mVU.regAlloc->clearNeeded(operand);
		mVU.regAlloc->clearNeeded(source);
		return;
	}

	mVUemitUpperSoftStackAlloc(stack_size);
	if (needs_result_flags)
	{
		xMOV(resultPtr(offsetof(VuSoftFmacJitResult, mac_flags)), 0);
		xMOV(resultPtr(offsetof(VuSoftFmacJitResult, mul_stage_status_flags)), 0);
	}
	if (op.WritesAcc())
		xMOV(resultPtr(offsetof(VuSoftFmacJitResult, acc_overflow_mask)), 0);
	xMOVAPS(ptr128[rsp + source_values], source);
	if (op.WritesAcc() && _XYZW_SS)
	{
		const int active_lane = _X ? 0 : (_Y ? 1 : (_Z ? 2 : 3));
		xMOVD(eax, accumulator);
		xMOV(ptr32[rsp + accumulator_values + active_lane * 4], eax);
	}
	else
	{
		xMOVAPS(ptr128[rsp + accumulator_values], accumulator);
	}
	if (variant == 0)
		xMOVAPS(ptr128[rsp + operand_values], operand);
	else
	{
		mVUemitUpperInlineExtractOperand(eax, operand, op, 0);
		for (int lane = 0; lane < 4; lane++)
			xMOV(ptr32[rsp + operand_values + lane * 4], eax);
	}

	xMOV(eax, ptr32[&mVU.regs().accflag]);
	xMOV(ptr32[rsp + incoming_acc_overflow], eax);

	if (variant != 0)
		mVUemitUpperInlineExtractOperand(r9d, operand, op, 0);

	// VPSRLVD/VPSLLVD make the packed helper AVX2-only. This condition is evaluated
	// while recompiling, so unsupported hosts fall through to the exact scalar path
	// without adding a branch to generated code.
	if (!switch_mxcsr && g_cpu.vectorISA >= ProcessorFeatures::VectorISA::AVX2)
	{
		xLEA(rax, ptr[rsp + source_values]);
		xLEA(rdx, ptr[rsp + operand_values]);
		xLEA(rcx, ptr[rsp + accumulator_values]);
		xMOV(r8d, ptr32[rsp + incoming_acc_overflow]);
		xLEA(r9, ptr[rsp + result_offset]);
		xMOV(r10d, static_cast<u32>(_X_Y_Z_W));
		xMOV(r11d, static_cast<u32>(s_vu_soft_lane_mask[_X_Y_Z_W]));
		xCALL(mVU.softMaddPacked[op.IsKind(VuUpperFmacSoftKind::Msub) ? 1 : 0]);
		xTEST(eax, eax);
		xForwardJZ32 outlined_common_failed;
		xMOVAPS(native_product, resultPtr(offsetof(VuSoftFmacJitResult, value)));
		outlined_result_ready.emplace();
		outlined_common_failed.SetTarget();
	}
	// The lane mask is known while recompiling. Emit one semiraw multiply/add
	// call per active lane instead of a four-iteration runtime search and avoid
	// materializing the truncated product between two helper calls. Scalar
	// instructions consequently emit exactly one helper call.
	for (int lane = 0; lane < 4; lane++)
	{
		const u32 lane_bit = 8u >> lane;
		if (!(static_cast<u32>(_X_Y_Z_W) & lane_bit))
			continue;
		xMOV(eax, ptr32[rsp + source_values + lane * 4]);
		xMOV(edx, ptr32[rsp + operand_values + lane * 4]);
		xMOV(ecx, ptr32[rsp + accumulator_values + lane * 4]);
		xMOV(r8d, ptr32[rsp + incoming_acc_overflow]);
		xAND(r8d, lane_bit);
		xMOV(r9d, op.IsKind(VuUpperFmacSoftKind::Msub) ? 1 : 0);
		xCALL(mVU.softMaddIntegratedLane);
		xMOV(resultPtr(offsetof(VuSoftFmacJitResult, value) + lane * 4), eax);
		xMOV(ptr32[rsp + lane_flags + lane * 4], edx);
		if (needs_result_flags)
			xOR(resultPtr(offsetof(VuSoftFmacJitResult, mul_stage_status_flags)), ecx);
		if (op.WritesAcc())
		{
			xTEST(edx, lane_overflow);
			xForwardJZ8 lane_did_not_overflow;
			xOR(resultPtr(offsetof(VuSoftFmacJitResult, acc_overflow_mask)), lane_bit);
			lane_did_not_overflow.SetTarget();
		}
	}
	if (use_packed_native)
	{
		xForwardJump32 exact_result_tail;
		if (outlined_result_ready.has_value())
			outlined_result_ready->SetTarget();
		mVUemitUpperInlineCommitResult(mVU, destination, native_product);

		if (needs_result_flags)
		{
			xMOVMSKPS(eax, native_product);
			xAND(eax, s_vu_soft_lane_mask[_X_Y_Z_W]);
			xMOVDZX(native_invalid, eax);
			xMOVAPS(native_power, ptr128[s_vu_soft_lane_mask]);
			xPSHUF.B(native_power, native_invalid);
			xMOVD(eax, native_power);
			xSHL(eax, 4);
			xMOV(resultPtr(offsetof(VuSoftFmacJitResult, mac_flags)), eax);
		}
		native_active_flags_ready.emplace();
		exact_result_tail.SetTarget();
	}

	for (int lane = 0; lane < 4; lane++)
	{
		if (!(static_cast<u32>(_X_Y_Z_W) & (8u >> lane)))
			continue;
		const u32 shift = 3 - lane;
		xMOV(eax, resultPtr(offsetof(VuSoftFmacJitResult, value) + lane * 4));
		xPINSR.D(destination, eax, _XYZW_SS ? 0 : lane);
		if (needs_result_flags)
		{
			xTEST(eax, 0x80000000);
			xForwardJZ8 result_lane_positive;
			xOR(resultPtr(offsetof(VuSoftFmacJitResult, mac_flags)), 0x0010u << shift);
			result_lane_positive.SetTarget();
			xTEST(ptr32[rsp + lane_flags + lane * 4], lane_underflow);
			xForwardJZ8 result_lane_not_underflow;
			xOR(resultPtr(offsetof(VuSoftFmacJitResult, mac_flags)), 0x0101u << shift);
			xForwardJump32 result_lane_flags_ready;
			result_lane_not_underflow.SetTarget();
			xMOV(edx, eax);
			xAND(edx, 0x7fffffff);
			xForwardJNZ8 result_lane_not_zero;
			xOR(resultPtr(offsetof(VuSoftFmacJitResult, mac_flags)), 0x0001u << shift);
			xForwardJump32 result_lane_flags_ready_from_zero;
			result_lane_not_zero.SetTarget();
			xTEST(ptr32[rsp + lane_flags + lane * 4], lane_overflow);
			xForwardJZ8 result_lane_flags_ready_nonexception;
			xOR(resultPtr(offsetof(VuSoftFmacJitResult, mac_flags)), 0x1000u << shift);
			result_lane_flags_ready.SetTarget();
			result_lane_flags_ready_from_zero.SetTarget();
			result_lane_flags_ready_nonexception.SetTarget();
		}
	}
	if (use_packed_native)
		native_active_flags_ready->SetTarget();

	const bool scalar_mask = _X_Y_Z_W == 0x1 || _X_Y_Z_W == 0x2 || _X_Y_Z_W == 0x4 || _X_Y_Z_W == 0x8;
	if (needs_result_flags && op.WritesAcc() && _X_Y_Z_W != 0xf && !scalar_mask)
	{
		for (int lane = 0; lane < 4; lane++)
		{
			const u32 lane_bit = 8u >> lane;
			if (static_cast<u32>(_X_Y_Z_W) & lane_bit)
				continue;
			const u32 shift = 3 - lane;
			xMOV(eax, ptr32[&mVU.regs().ACC.UL[lane]]);
			xTEST(eax, 0x80000000);
			xForwardJZ8 inactive_lane_positive;
			xOR(resultPtr(offsetof(VuSoftFmacJitResult, mac_flags)), 0x0010u << shift);
			inactive_lane_positive.SetTarget();
			xAND(eax, 0x7fffffff);
			xForwardJNZ8 inactive_lane_nonzero;
			xOR(resultPtr(offsetof(VuSoftFmacJitResult, mac_flags)), 0x0001u << shift);
			xForwardJump8 inactive_lane_flags_ready;
			inactive_lane_nonzero.SetTarget();
			xTEST(ptr32[rsp + incoming_acc_overflow], lane_bit);
			xForwardJZ8 inactive_lane_flags_ready_nonoverflow;
			xOR(resultPtr(offsetof(VuSoftFmacJitResult, mac_flags)), 0x1000u << shift);
			inactive_lane_flags_ready.SetTarget();
			inactive_lane_flags_ready_nonoverflow.SetTarget();
		}
	}
	if (needs_result_flags)
	{
		mVUemitUpperStatusFromMacFlags(
			resultPtr(offsetof(VuSoftFmacJitResult, mac_flags)),
			resultPtr(offsetof(VuSoftFmacJitResult, status_flags)));
		xOR(ecx, resultPtr(offsetof(VuSoftFmacJitResult, mul_stage_status_flags)));
		xMOV(resultPtr(offsetof(VuSoftFmacJitResult, sticky_status_flags)), ecx);
	}
	if (op.WritesAcc())
	{
		mVUemitUpperInlineSoftAccOverflowWriteback(mVU, result_offset);
	}
	mVUemitSoftFlagWriteback(mVU, result_offset, op);
	mVUemitUpperSoftStackFree(stack_size);
	if (register_common_finished.has_value())
		register_common_finished->SetTarget();
	if (compact_common_finished.has_value())
		compact_common_finished->SetTarget();
	mVU.regAlloc->clearNeeded(destination);
	if (destination.Id != accumulator.Id)
		mVU.regAlloc->clearNeeded(accumulator);
	if (use_packed_native)
	{
		mVU.regAlloc->clearNeeded(native_power);
		mVU.regAlloc->clearNeeded(native_invalid);
		mVU.regAlloc->clearNeeded(native_work);
		mVU.regAlloc->clearNeeded(native_operand);
		mVU.regAlloc->clearNeeded(native_product);
	}
	mVU.regAlloc->clearNeeded(operand);
	mVU.regAlloc->clearNeeded(source);
}

static void mVUemitUpperSoftExact(microVU& mVU, VuUpperFmacSoftDescriptor op)
{
	if (op.IsAddSub())
	{
		mVUemitUpperInlineAddSubExactResult(mVU, op);
		mVU.regAlloc->markSoftNonExtended(op.WritesAcc() ? mVUsoftAccRegisterIndex : _Fd_, _X_Y_Z_W);
		return;
	}
	mVU.regAlloc->flushCallerSavedGPRs();
	const int variant = op.OperandVariant();
	const xmm& source = mVU.regAlloc->allocReg(_Fs_);
	xmm operand;
	if (variant == 1)
	{
		operand = mVU.regAlloc->allocReg(33);
	}
	else if (variant == 2)
	{
		operand = mVU.regAlloc->allocReg();
		getQreg(operand, mVUinfo.readQ);
	}
	else
	{
		operand = mVU.regAlloc->allocReg(_Ft_);
	}
	if (op.ReadsQ())
	{
		xMOVSS(ptr32[&mVU.regs().VI[REG_Q].UL], operand);
	}
	if (op.IsKind(VuUpperFmacSoftKind::Mul))
	{
		const int destination_reg = op.WritesAcc() ? mVUsoftAccRegisterIndex : _Fd_;
		const int destination_load = _X_Y_Z_W == 0xf ? -1 : destination_reg;
		const xmm& destination = mVU.regAlloc->allocReg(destination_load, destination_reg, _X_Y_Z_W);
		mVUemitUpperInlineMulExactResult(mVU, op, source, operand, destination);
		mVU.regAlloc->markSoftNonExtended(destination_reg, _X_Y_Z_W);
		return;
	}
	if (op.IsMultiplyAdd())
	{
		const u8 source_mask = static_cast<u8>(_X_Y_Z_W);
		const u8 operand_mask = variant == 0 ? source_mask :
		                                       (variant >= 3 ? static_cast<u8>(8u >> (variant - 3)) : 0);
		bool source_nonextended =
			(mVU.regAlloc->getSoftNonExtendedMask(_Fs_) & source_mask) == source_mask;
		bool operand_nonextended = operand_mask != 0 &&
		                           (mVU.regAlloc->getSoftNonExtendedMask(_Ft_) & operand_mask) == operand_mask;
		// Use allocator-local facts only when they cover both inputs. Partial facts
		// do not prove that the complete exceptional-input guard can be omitted.
		const bool inputs_nonextended = source_nonextended && operand_nonextended;
		source_nonextended = inputs_nonextended;
		operand_nonextended = inputs_nonextended;
		const xmm& accumulator = mVU.regAlloc->allocReg(32);
		const bool accumulator_nonextended =
			mVU.regAlloc->getSoftNonExtendedMask(mVUsoftAccRegisterIndex) == 0xf;
		const int destination_reg = op.WritesAcc() ? 32 : _Fd_;
		const int destination_load = op.WritesAcc() ? 32 : (_X_Y_Z_W == 0xf ? -1 : _Fd_);
		const xmm& destination = mVU.regAlloc->allocReg(destination_load, destination_reg, _X_Y_Z_W,
			!op.WritesAcc());
		mVUemitUpperInlineMaddExactResult(mVU, op, source, operand,
			accumulator, destination, true, accumulator_nonextended,
			source_nonextended, operand_nonextended, false);
		mVU.regAlloc->markSoftNonExtended(destination_reg, _X_Y_Z_W);
		return;
	}
	pxFailRel("Unsupported upper softfloat operation reached exact lowering.");
	return;
}



static bool mVUdecodeBroadcastDotProductOp(u32 code, VuUpperFmacSoftDescriptor* op)
{
	const u32 opcode = code & 0x3f;
	const u32 sub_opcode = (code >> 6) & 0x1f;
	const u32 variant = code & 0x3;
	const VuUpperFmacSoftOperandSource source = variant == 0 ? VuUpperFmacSoftOperandSource::X :
	                                            variant == 1 ? VuUpperFmacSoftOperandSource::Y :
	                                            variant == 2 ? VuUpperFmacSoftOperandSource::Z :
	                                                           VuUpperFmacSoftOperandSource::W;

	if ((opcode & 0x3c) == 0x3c && sub_opcode == 6)
	{
		*op = {VuUpperFmacSoftKind::Mul, source, VuUpperFmacSoftDestination::Acc};
		return true;
	}
	if ((opcode & 0x3c) == 0x3c && sub_opcode == 2)
	{
		*op = {VuUpperFmacSoftKind::Madd, source, VuUpperFmacSoftDestination::Acc};
		return true;
	}
	if ((opcode & 0x3c) == 0x08)
	{
		*op = {VuUpperFmacSoftKind::Madd, source, VuUpperFmacSoftDestination::Fd};
		return true;
	}
	return false;
}



struct mVUUpperSoftRegisterDotFusion
{
	static constexpr int MAX_LENGTH = 4;
	std::array<VuUpperFmacSoftDescriptor, MAX_LENGTH> ops = {};
	std::array<u32, MAX_LENGTH> codes = {};
	int length = 0;
};

static bool mVUupperSoftIsBroadcastOp(VuUpperFmacSoftDescriptor op, VuUpperFmacSoftKind kind,
	VuUpperFmacSoftDestination destination)
{
	return op.IsKind(kind) && op.destination == destination && op.UsesBroadcastOperand();
}

static bool mVUfindUpperSoftRegisterDotFusion(microVU& mVU, u32 first_pc,
	VuUpperFmacSoftDescriptor first_op, mVUUpperSoftRegisterDotFusion* fusion)
{
	if (!mVUupperSoftIsBroadcastOp(first_op, VuUpperFmacSoftKind::Mul, VuUpperFmacSoftDestination::Acc) ||
		mVUupperSoftNeedsTruncateMxcsr(mVU))
	{
		return false;
	}

	for (int index = 0; index < mVUUpperSoftRegisterDotFusion::MAX_LENGTH; index++)
	{
		const u32 pc = first_pc + index * 2;
		if (pc >= mVU.progSize)
			return false;
		fusion->codes[index] = reinterpret_cast<const u32*>(mVU.regs().Micro)[pc];
		if (!mVUdecodeBroadcastDotProductOp(fusion->codes[index], &fusion->ops[index]) ||
			((fusion->codes[index] >> 21) & 0xf) != 0xf ||
			(index == 0 && fusion->ops[index] != first_op))
		{
			return false;
		}
		if (index > 0 && mVUupperSoftIsBroadcastOp(
							 fusion->ops[index], VuUpperFmacSoftKind::Madd, VuUpperFmacSoftDestination::Fd))
		{
			if (index < 2)
				return false;
			fusion->length = index + 1;
			break;
		}
		if (index > 0 && !mVUupperSoftIsBroadcastOp(
							 fusion->ops[index], VuUpperFmacSoftKind::Madd, VuUpperFmacSoftDestination::Acc))
		{
			return false;
		}
	}
	if (fusion->length == 0)
		return false;

	for (int index = 0; index < fusion->length; index++)
	{
		const microOp& info = mVUir.info[(first_pc + index * 2) / 2];
		if (info.swapOps || info.backupVF || info.isEOB || info.isBdelay || info.doXGKICK ||
			info.doDivFlag || info.lOp.readFlags || info.lOp.branch || info.uOp.eBit ||
			info.uOp.mBit || info.uOp.tBit || info.uOp.dBit)
		{
			return false;
		}

		const int lower_write = info.lOp.VF_write.reg;
		for (int later = index + 1; later < fusion->length; later++)
		{
			const int later_fs = (fusion->codes[later] >> 11) & 0x1f;
			const int later_ft = (fusion->codes[later] >> 16) & 0x1f;
			const bool later_writes_fd = mVUupperSoftIsBroadcastOp(
				fusion->ops[later], VuUpperFmacSoftKind::Madd, VuUpperFmacSoftDestination::Fd);
			const int later_fd = later_writes_fd ? ((fusion->codes[later] >> 6) & 0x1f) : 0;
			if (lower_write != 0 &&
				(lower_write == later_fs || lower_write == later_ft || lower_write == later_fd))
			{
				return false;
			}
			if (later_fd != 0)
			{
				for (const microVFreg& lower_read : info.lOp.VF_read)
				{
					if (lower_read.reg == later_fd)
						return false;
				}
			}
		}
	}
	return true;
}

static bool mVUtryStartUpperSoftRegisterDotFusion(microVU& mVU, VuUpperFmacSoftDescriptor first_op)
{
	const u32 first_pc = iPC;
	const u32 first_code = mVU.code;
	mVUUpperSoftRegisterDotFusion fusion;
	if (!mVUfindUpperSoftRegisterDotFusion(mVU, first_pc, first_op, &fusion))
		return false;

	for (int index = 0; index < fusion.length; index++)
	{
		iPC = first_pc + index * 2;
		mVU.code = fusion.codes[index];
		mVUemitUpperSoftExact(mVU, fusion.ops[index]);
	}
	iPC = first_pc;
	mVU.code = first_code;
	return true;
}

static bool mVUisUpperSoftRegisterDotFusionContinuation(microVU& mVU, VuUpperFmacSoftDescriptor op)
{
	const u32 instructions_since_block_start =
		((iPC - mVUstartPC) & mVU.progMemMask) / 2;
	for (int stage = 1; stage < mVUUpperSoftRegisterDotFusion::MAX_LENGTH; stage++)
	{
		if (instructions_since_block_start < static_cast<u32>(stage))
			break;

		const u32 first_pc = (iPC - stage * 2) & mVU.progMemMask;
		VuUpperFmacSoftDescriptor first_op;
		if (!mVUdecodeBroadcastDotProductOp(
				reinterpret_cast<const u32*>(mVU.regs().Micro)[first_pc], &first_op))
		{
			continue;
		}

		mVUUpperSoftRegisterDotFusion fusion;
		if (mVUfindUpperSoftRegisterDotFusion(mVU, first_pc, first_op, &fusion) &&
			stage < fusion.length && fusion.codes[stage] == mVU.code && fusion.ops[stage] == op)
		{
			return true;
		}
	}
	return false;
}
