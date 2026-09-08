// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

//------------------------------------------------------------------
// Micro VU Micromode Lower instructions
//------------------------------------------------------------------

//------------------------------------------------------------------
// DIV/SQRT/RSQRT
//------------------------------------------------------------------

struct mVUSoftDivCapTailPatch
{
	std::optional<xForwardJump32> entry;
	const u8* srt_resume = nullptr;
	const u8* quotient_ready = nullptr;
};

static constexpr sptr mVUsoftUnaryCacheKeyOffset = offsetof(microVUSoftUnaryCacheEntry, key);
static constexpr sptr mVUsoftLowerCacheKeyAOffset = offsetof(microVUSoftLowerCacheEntry, key_a);

static void mVUemitLowerSoftUnaryCacheIndex(const xRegister32& index, const xRegister32& key)
{
	static_assert(mVUsoftLowerCacheSize == 4096);
	xMUL(index, key, static_cast<s32>(2654435761u));
	xSHR(index, 20);
}

static void mVUemitLowerSoftBinaryCacheIndex(
	const xRegister32& index, const xRegister32& key_a, const xRegister32& key_b)
{
	xMUL(index, key_b, static_cast<s32>(2246822519u));
	xXOR(index, key_a);
	mVUemitLowerSoftUnaryCacheIndex(index, index);
}

template <typename CacheEntry>
static void mVUemitLowerSoftCacheAddress(const CacheEntry* cache)
{
	static_assert(sizeof(CacheEntry) == 16 || sizeof(CacheEntry) == 32);
	constexpr u8 entry_shift = sizeof(CacheEntry) == 16 ? 4 : 5;
	xSHL(ecx, entry_shift);
	xMOV64(r11, reinterpret_cast<uptr>(cache));
	xADD(r11, rcx);
}

static void mVUemitLowerSoftCacheSetAddress(const microVUSoftLowerCacheSet* cache)
{
	xSHL(ecx, 6);
	xMOV64(r11, reinterpret_cast<uptr>(cache));
	xADD(r11, rcx);
}

static void mVUemitLowerSoftQAndStatusWriteback(microVU& mVU)
{
	// Internal exact-kernel ABI: eax = raw result, edx = current I/D exception bits.
	xMOV(ptr32[&mVU.regs().q.UL], eax);
	const xmm& q_result = mVU.regAlloc->allocReg();
	xMOVDZX(q_result, eax);
	writeQreg(q_result, mVUinfo.writeQ);
	mVU.regAlloc->clearNeeded(q_result);

	xMOV(ecx, edx);
	xSHL(ecx, 14);
	xMOV(eax, ecx);
	xSHL(eax, 6);
	xOR(ecx, eax);
	xMOV(ptr32[&mVU.divFlag], ecx);
	// Micro mode commits these causes through mVUdivSet when Q matures.
	if (mVU.cop2)
	{
		xAND(gprF0, ~0xc0000u);
		xSHL(edx, 14);
		xOR(gprF0, edx);
		xSHL(edx, 6);
		xOR(gprF0, edx);
	}
}

static void mVUGenerateLowerSrtReciprocalSoftExactKernel(microVU& mVU)
{
	// Internal ABI: eax = raw divisor. Returns eax = raw 1.0/divisor using
	// the P unit's SRT quotient and edx = current I/D exception bits.
	constexpr sptr input_raw = 0;
	constexpr int result_raw = input_raw + 4;
	constexpr int exception = result_raw + 4;
	constexpr int result_exp = exception + 4;
	constexpr int stack_size = result_exp + 8;

	mVU.softSrtReciprocalExact = xGetAlignedCallTarget();
	xSUB(rsp, stack_size);
	xMOV(ptr32[rsp + input_raw], eax);
	mVUemitLowerSoftUnaryCacheIndex(ecx, eax);
	mVUemitLowerSoftCacheAddress(mVU.softSrtReciprocalCache.get());
	xCMP(ptr32[r11 + offsetof(microVUSoftUnaryCacheEntry, valid)], 0);
	xForwardJZ32 srt_reciprocal_cache_miss_invalid;
	xCMP(eax, ptr32[r11 + mVUsoftUnaryCacheKeyOffset]);
	xForwardJNE32 srt_reciprocal_cache_miss_key;
	xMOV(eax, ptr32[r11 + offsetof(microVUSoftUnaryCacheEntry, result)]);
	xMOV(edx, ptr32[r11 + offsetof(microVUSoftUnaryCacheEntry, exception)]);
	xForwardJump32 srt_reciprocal_finished;

	srt_reciprocal_cache_miss_invalid.SetTarget();
	srt_reciprocal_cache_miss_key.SetTarget();
	xMOV(edx, eax);
	xAND(edx, 0x7f800000);
	xForwardJZ32 srt_reciprocal_divisor_zero;

	xSHR(edx, 23);
	xMOV(ecx, 253);
	xSUB(ecx, edx);
	xMOV(ptr32[rsp + result_exp], ecx);
	xCMP(ecx, 0);
	xForwardJL32 srt_reciprocal_underflow_initial;
	// For a normalized 1.0 numerator, exhaustive characterization proves that
	// the exact SRT quotient is the ordinary truncated 48/24 quotient plus one
	// divisor-mantissa-indexed correction bit. Keep the established SRT packing
	// below so normalization, exponent transitions, sign, and underflow remain
	// identical to the recurrence path.
	xMOV(r11d, ptr32[rsp + input_raw]);
	xAND(r11d, 0x7fffff);
	xOR(r11d, 0x800000);
	xMOV64(rax, 0x800000000000ULL);
	xXOR(edx, edx);
	xUDIV(r11);

	xMOV(ecx, ptr32[rsp + input_raw]);
	xAND(ecx, 0x7fffff);
	xMOV64(r10, reinterpret_cast<uptr>(MicroVUSoftFloatTables::reciprocal_correction_lookup));
	xXOR(r9d, r9d);
	xBT(ptr32[r10], ecx);
	xSETB(r9b);
	xADD(eax, r9d);
	xCMP(eax, 1 << 24);
	xForwardJL8 srt_reciprocal_quotient_normalized;
	xSHR(eax, 1);
	xINC(ptr32[rsp + result_exp]);
	srt_reciprocal_quotient_normalized.SetTarget();
	xMOV(edx, ptr32[rsp + result_exp]);
	xCMP(edx, 1);
	xForwardJL32 srt_reciprocal_underflow_normalized;
	xAND(eax, 0x7fffff);
	xSHL(edx, 23);
	xOR(eax, edx);
	xMOV(edx, ptr32[rsp + input_raw]);
	xAND(edx, 0x80000000);
	xOR(eax, edx);
	xXOR(edx, edx);
	xForwardJump32 srt_reciprocal_result_ready_normal;

	srt_reciprocal_divisor_zero.SetTarget();
	xMOV(eax, ptr32[rsp + input_raw]);
	xAND(eax, 0x80000000);
	xOR(eax, 0x7fffffff);
	xMOV(edx, 0x20);
	xForwardJump32 srt_reciprocal_result_ready_zero;

	srt_reciprocal_underflow_initial.SetTarget();
	srt_reciprocal_underflow_normalized.SetTarget();
	xMOV(eax, ptr32[rsp + input_raw]);
	xAND(eax, 0x80000000);
	xXOR(edx, edx);

	srt_reciprocal_result_ready_normal.SetTarget();
	srt_reciprocal_result_ready_zero.SetTarget();
	xMOV(ptr32[rsp + result_raw], eax);
	xMOV(ptr32[rsp + exception], edx);
	xMOV(eax, ptr32[rsp + input_raw]);
	mVUemitLowerSoftUnaryCacheIndex(ecx, eax);
	mVUemitLowerSoftCacheAddress(mVU.softSrtReciprocalCache.get());
	xMOV(ptr32[r11 + mVUsoftUnaryCacheKeyOffset], eax);
	xMOV(eax, ptr32[rsp + result_raw]);
	xMOV(ptr32[r11 + offsetof(microVUSoftUnaryCacheEntry, result)], eax);
	xMOV(edx, ptr32[rsp + exception]);
	xMOV(ptr32[r11 + offsetof(microVUSoftUnaryCacheEntry, exception)], edx);
	xMOV(ptr32[r11 + offsetof(microVUSoftUnaryCacheEntry, valid)], 1);

	srt_reciprocal_finished.SetTarget();
	xADD(rsp, stack_size);
	xRET();
}

static mVUSoftDivCapTailPatch mVUGenerateLowerDivSoftExactKernel(microVU& mVU)
{
	// Internal ABI: eax = raw Fs lane, edx = raw Ft lane. Returns eax = raw
	// result and edx = current I/D exception bits. DIV and RSQRT both retain the
	// VU divider's final redundant SRT digit.
	constexpr sptr fs_raw = 0;
	constexpr int ft_raw = fs_raw + 4;
	constexpr int result_raw = ft_raw + 4;
	constexpr int exception = result_raw + 4;
	constexpr int result_exp = exception + 4;
	constexpr int saved_rbp = result_exp + 8;
	constexpr int saved_rsi = saved_rbp + 8;
	constexpr int stack_size = saved_rsi + 8;
	mVUSoftDivCapTailPatch cap_tail;

	mVU.softDivExact = xGetAlignedCallTarget();
	xSUB(rsp, stack_size);
	xMOV(ptr32[rsp + fs_raw], eax);
	xMOV(ptr32[rsp + ft_raw], edx);
	xMOV(ptr64[rsp + saved_rbp], rbp);
	xMOV(ptr64[rsp + saved_rsi], rsi);
	mVUemitLowerSoftBinaryCacheIndex(ecx, eax, edx);
	mVUemitLowerSoftCacheAddress(mVU.softDivCache.get());
	xCMP(ptr32[r11 + offsetof(microVUSoftLowerCacheEntry, valid)], 0);
	xForwardJZ32 division_cache_miss_invalid;
	xCMP(eax, ptr32[r11 + mVUsoftLowerCacheKeyAOffset]);
	xForwardJNE32 division_cache_miss_a;
	xCMP(edx, ptr32[r11 + offsetof(microVUSoftLowerCacheEntry, key_b)]);
	xForwardJNE32 division_cache_miss_b;
	xMOV(eax, ptr32[r11 + offsetof(microVUSoftLowerCacheEntry, result)]);
	xMOV(edx, ptr32[r11 + offsetof(microVUSoftLowerCacheEntry, exception)]);
	xForwardJump32 division_cache_result_ready;
	division_cache_miss_invalid.SetTarget();
	division_cache_miss_a.SetTarget();
	division_cache_miss_b.SetTarget();

	xMOV(eax, ptr32[rsp + fs_raw]);
	xAND(eax, 0x7f800000);
	xMOV(edx, ptr32[rsp + ft_raw]);
	xAND(edx, 0x7f800000);
	xTEST(edx, edx);
	xForwardJNZ32 divisor_normal;
	xMOV(eax, ptr32[rsp + fs_raw]);
	xXOR(eax, ptr32[rsp + ft_raw]);
	xAND(eax, 0x80000000);
	xOR(eax, 0x7fffffff);
	xTEST(ptr32[rsp + fs_raw], 0x7f800000);
	xForwardJNZ8 divide_by_zero;
	xMOV(edx, 0x10);
	xForwardJump32 result_ready_from_invalid;
	divide_by_zero.SetTarget();
	xMOV(edx, 0x20);
	xForwardJump32 result_ready_from_divide;

	divisor_normal.SetTarget();
	xTEST(eax, eax);
	xForwardJNZ32 dividend_normal;
	xMOV(eax, ptr32[rsp + fs_raw]);
	xXOR(eax, ptr32[rsp + ft_raw]);
	xAND(eax, 0x80000000);
	xXOR(edx, edx);
	xForwardJump32 result_ready_from_zero;

	dividend_normal.SetTarget();
	xMOV(eax, ptr32[rsp + fs_raw]);
	xSHR(eax, 23);
	xAND(eax, 0xff);
	xMOV(edx, ptr32[rsp + ft_raw]);
	xSHR(edx, 23);
	xAND(edx, 0xff);
	xSUB(eax, edx);
	xADD(eax, 126);
	xMOV(ptr32[rsp + result_exp], eax);
	xCMP(eax, 255);
	xForwardJG32 division_overflow;
	xCMP(eax, 0);
	xForwardJL32 division_underflow;


	// Keep the probe body outside the program-code region so this path does not
	// shift any shared helper or guest-program entry. The original SRT path
	// begins with a three-byte MOV and five-byte AND; replace those eight bytes
	// with the five-byte tail jump plus padding, then restore the displaced
	// instructions before resuming the byte-for-byte recurrence on fallback.
	cap_tail.entry.emplace();
	xNOP();
	xNOP();
	xNOP();
	cap_tail.srt_resume = xGetPtr();
	xOR(eax, 0x800000);
	xSHL(eax, 2);
	xMOV(r9d, eax);
	xMOV(eax, ptr32[rsp + ft_raw]);
	xAND(eax, 0x7fffff);
	xOR(eax, 0x800000);
	xSHL(eax, 2);
	xMOV(r11d, eax);
	xXOR(r10d, r10d);
	xXOR(ebp, ebp);
	xMOV(r8d, 1);

	xMOV(esi, 23);
	u8* const quotient_loop = xGetPtr();
	xSHL(ebp, 1);
	xADD(ebp, r8d);
	X86SoftFloatEmitter::EmitDivCarrySaveStep();
	xDEC(esi);
	xJcc32(Jcc_NotZero,
		static_cast<s32>(reinterpret_cast<sptr>(quotient_loop) - (reinterpret_cast<sptr>(xGetPtr()) + 6)));

	xSHL(ebp, 1);
	xADD(ebp, r8d);
	X86SoftFloatEmitter::EmitDivCarrySaveStep();
	xMOV(eax, ebp);
	xSHL(eax, 1);
	xADD(eax, r8d);
	xCMP(eax, 1 << 24);
	xForwardJL8 srt_quotient_normalized;
	xSHR(eax, 1);
	xINC(ptr32[rsp + result_exp]);
	srt_quotient_normalized.SetTarget();
	cap_tail.quotient_ready = xGetPtr();
	xMOV(edx, ptr32[rsp + result_exp]);
	xCMP(edx, 255);
	xForwardJG32 division_overflow_after_normalize;
	xCMP(edx, 1);
	xForwardJL32 division_underflow_after_normalize;
	xAND(eax, 0x7fffff);
	xSHL(edx, 23);
	xOR(eax, edx);
	xMOV(ecx, ptr32[rsp + fs_raw]);
	xXOR(ecx, ptr32[rsp + ft_raw]);
	xAND(ecx, 0x80000000);
	xOR(eax, ecx);
	xXOR(edx, edx);
	xForwardJump32 result_ready;

	division_overflow.SetTarget();
	division_overflow_after_normalize.SetTarget();
	xMOV(eax, ptr32[rsp + fs_raw]);
	xXOR(eax, ptr32[rsp + ft_raw]);
	xAND(eax, 0x80000000);
	xOR(eax, 0x7fffffff);
	xXOR(edx, edx);
	xForwardJump32 result_ready_from_overflow;

	division_underflow.SetTarget();
	division_underflow_after_normalize.SetTarget();
	xMOV(eax, ptr32[rsp + fs_raw]);
	xXOR(eax, ptr32[rsp + ft_raw]);
	xAND(eax, 0x80000000);
	xXOR(edx, edx);

	result_ready.SetTarget();
	result_ready_from_overflow.SetTarget();
	result_ready_from_zero.SetTarget();
	result_ready_from_invalid.SetTarget();
	result_ready_from_divide.SetTarget();
	xMOV(ptr32[rsp + result_raw], eax);
	xMOV(ptr32[rsp + exception], edx);
	xMOV(eax, ptr32[rsp + fs_raw]);
	xMOV(edx, ptr32[rsp + ft_raw]);
	mVUemitLowerSoftBinaryCacheIndex(ecx, eax, edx);
	mVUemitLowerSoftCacheAddress(mVU.softDivCache.get());
	xMOV(ptr32[r11 + mVUsoftLowerCacheKeyAOffset], eax);
	xMOV(ptr32[r11 + offsetof(microVUSoftLowerCacheEntry, key_b)], edx);
	xMOV(eax, ptr32[rsp + result_raw]);
	xMOV(ptr32[r11 + offsetof(microVUSoftLowerCacheEntry, result)], eax);
	xMOV(edx, ptr32[rsp + exception]);
	xMOV(ptr32[r11 + offsetof(microVUSoftLowerCacheEntry, exception)], edx);
	xMOV(ptr32[r11 + offsetof(microVUSoftLowerCacheEntry, valid)], 1);
	division_cache_result_ready.SetTarget();
	xMOV(rbp, ptr64[rsp + saved_rbp]);
	xMOV(rsi, ptr64[rsp + saved_rsi]);
	xADD(rsp, stack_size);
	xRET();
	return cap_tail;
}

static void mVUGenerateLowerDivSoftCapTail(const mVUSoftDivCapTailPatch& cap_tail)
{
	constexpr sptr fs_raw = 0;
	constexpr int ft_raw = fs_raw + 4;
	constexpr int result_raw = ft_raw + 4;
	constexpr int exception = result_raw + 4;
	constexpr int result_exp = exception + 4;
	static_assert(result_exp == 16);
	pxAssert(cap_tail.entry.has_value());
	pxAssert(cap_tail.srt_resume && cap_tail.quotient_ready);
	cap_tail.entry->SetTarget();

	xMOV(r9d, ptr32[rsp + fs_raw]);
	xAND(r9d, 0x7fffff);
	xOR(r9d, 0x800000);
	xMOV(r11d, ptr32[rsp + ft_raw]);
	xAND(r11d, 0x7fffff);
	xOR(r11d, 0x800000);
	X86SoftFloatEmitter::EmitSrtDivCapQuotient();
	xCMP(ecx, edx);
	xForwardJA32 cap_safe;
	xMOV(eax, ptr32[rsp + fs_raw]);
	xAND(eax, 0x7fffff);
	xJMP(cap_tail.srt_resume);
	cap_safe.SetTarget();
	xTEST(r8d, r8d);
	xForwardJNZ8 exponent_ready;
	xINC(ptr32[rsp + result_exp]);
	exponent_ready.SetTarget();
	xJMP(cap_tail.quotient_ready);
}

static void mVUGenerateLowerSqrtSoftExactKernel(microVU& mVU)
{
	// Internal ABI: eax = raw Ft lane. Returns eax = raw result and edx = current
	// I/D exception bits.
	constexpr sptr ft_raw = 0;
	constexpr int exception = ft_raw + 4;
	constexpr int result_raw = exception + 4;
	constexpr int saved_rbp = 16;
	constexpr int saved_rsi = saved_rbp + 8;
	constexpr int saved_rdi = saved_rsi + 8;
	constexpr int saved_xmm = saved_rdi + 8;
	constexpr int stack_size = saved_xmm + 16 + 8;

	mVU.softSqrtExact = xGetAlignedCallTarget();
	xSUB(rsp, stack_size);
	xMOVAPS(ptr128[rsp + saved_xmm], xmm0);
	xMOV(ptr32[rsp + ft_raw], eax);
	mVUemitLowerSoftUnaryCacheIndex(ecx, eax);
	mVUemitLowerSoftCacheAddress(mVU.softSqrtCache.get());
	xCMP(ptr32[r11 + offsetof(microVUSoftUnaryCacheEntry, valid)], 0);
	xForwardJZ32 sqrt_cache_miss_invalid;
	xCMP(eax, ptr32[r11 + mVUsoftUnaryCacheKeyOffset]);
	xForwardJNE32 sqrt_cache_miss_key;
	xMOV(eax, ptr32[r11 + offsetof(microVUSoftUnaryCacheEntry, result)]);
	xMOV(edx, ptr32[r11 + offsetof(microVUSoftUnaryCacheEntry, exception)]);
	xForwardJump32 sqrt_cache_result_ready;
	sqrt_cache_miss_invalid.SetTarget();
	sqrt_cache_miss_key.SetTarget();
	xXOR(edx, edx);
	xTEST(eax, 0x80000000);
	xForwardJZ8 sqrt_status_ready;
	xMOV(edx, 0x10);
	sqrt_status_ready.SetTarget();
	xMOV(ptr32[rsp + exception], edx);
	xTEST(eax, 0x7f800000);
	xForwardJNZ32 sqrt_normal;
	xXOR(eax, eax);
	xForwardJump32 sqrt_result_ready_from_zero;

	sqrt_normal.SetTarget();
	xMOV(edx, ptr32[rsp + ft_raw]);
	xAND(edx, 0x7f800000);
	xCMP(edx, 0x7f800000);
	xForwardJZ32 sqrt_extended_input;
	xMOVDZX(xmm0, ptr32[rsp + ft_raw]);
	xPAND(xmm0, ptr128[mVUglob.absclip]);
	const bool switch_mxcsr = mVUupperSoftNeedsTruncateMxcsr(mVU);
	if (switch_mxcsr)
		xLDMXCSR(ptr32[&s_vu_soft_truncate_mxcsr]);
	xSQRT.SS(xmm0, xmm0);
	if (switch_mxcsr)
		xLDMXCSR(ptr32[mVU.index == 0 ? &EmuConfig.Cpu.VU0FPCR.bitmask : &EmuConfig.Cpu.VU1FPCR.bitmask]);
	xMOVD(r9d, xmm0);
	xTEST(ptr32[rsp + ft_raw], 0x7fffff);
	xForwardJZ32 sqrt_correction_ready;
	// SQRTSS with chop rounding supplies floor(sqrt(radicand)). Hardware never
	// applies the PS2 SRT +1 correction below the exact midpoint between this
	// root and the next. Prove that lower half with a 24x24->48 square and avoid
	// the correction bitmap; retain the bitmap for the representation-sensitive
	// upper half.
	xMOV(eax, r9d);
	xAND(eax, 0x7fffff);
	xOR(eax, 0x800000);
	xMOV(r8d, eax);
	xUMUL(r8);
	xADD(rax, r8);
	xMOV(r10d, ptr32[rsp + ft_raw]);
	xAND(r10d, 0x7fffff);
	xOR(r10d, 0x800000);
	xSHL(r10, 23);
	xTEST(ptr32[rsp + ft_raw], 0x800000);
	xForwardJNZ8 sqrt_residual_radicand_ready;
	xADD(r10, r10);
	sqrt_residual_radicand_ready.SetTarget();
	xCMP(r10, rax);
	xForwardJBE8 sqrt_residual_floor;
	xMOV(edx, ptr32[rsp + ft_raw]);
	xMOV(ecx, edx);
	xAND(ecx, 0x7fffff);
	xSHR(edx, 23);
	xAND(edx, 1);
	xXOR(edx, 1);
	xSHL(edx, 23);
	xOR(ecx, edx);
	xMOV64(r10, reinterpret_cast<uptr>(MicroVUSoftFloatTables::sqrt_correction_lookup));
	xXOR(eax, eax);
	xBT(ptr32[r10], ecx);
	xSETB(al);
	xADD(eax, r9d);
	xForwardJump8 sqrt_corrected_result_ready;
	sqrt_residual_floor.SetTarget();
	xMOV(eax, r9d);
	xForwardJump8 sqrt_residual_result_ready;
	sqrt_correction_ready.SetTarget();
	xMOV(eax, r9d);
	sqrt_residual_result_ready.SetTarget();
	sqrt_corrected_result_ready.SetTarget();
	xForwardJump32 sqrt_result_ready_from_fast;

	sqrt_extended_input.SetTarget();
	xMOV(ptr64[rsp + saved_rbp], rbp);
	xMOV(ptr64[rsp + saved_rsi], rsi);
	xMOV(ptr64[rsp + saved_rdi], rdi);
	xMOV(eax, ptr32[rsp + ft_raw]);
	xAND(eax, 0x7fffff);
	xOR(eax, 0x800000);
	xSHL(eax, 1);
	xTEST(ptr32[rsp + ft_raw], 0x800000);
	xForwardJNZ8 sqrt_mantissa_ready;
	xSHL(eax, 1);
	sqrt_mantissa_ready.SetTarget();
	xMOV(r9d, eax);
	xXOR(r10d, r10d);
	xXOR(ebp, ebp);
	xMOV(r8d, 1);

	xXOR(esi, esi);
	u8* const sqrt_loop = xGetPtr();
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
	xJcc32(Jcc_Less, static_cast<s32>(reinterpret_cast<sptr>(sqrt_loop) - (reinterpret_cast<sptr>(xGetPtr()) + 6)));

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
	xMOV(edx, ptr32[rsp + ft_raw]);
	xSHR(edx, 23);
	xAND(edx, 0xff);
	xADD(edx, 127);
	xSHR(edx, 1);
	xSHL(edx, 23);
	xOR(eax, edx);
	xMOV(rbp, ptr64[rsp + saved_rbp]);
	xMOV(rsi, ptr64[rsp + saved_rsi]);
	xMOV(rdi, ptr64[rsp + saved_rdi]);

	sqrt_result_ready_from_zero.SetTarget();
	sqrt_result_ready_from_fast.SetTarget();
	xMOV(ptr32[rsp + result_raw], eax);
	xMOV(eax, ptr32[rsp + ft_raw]);
	xMOV(ptr32[r11 + mVUsoftUnaryCacheKeyOffset], eax);
	xMOV(eax, ptr32[rsp + result_raw]);
	xMOV(ptr32[r11 + offsetof(microVUSoftUnaryCacheEntry, result)], eax);
	xMOV(edx, ptr32[rsp + exception]);
	xMOV(ptr32[r11 + offsetof(microVUSoftUnaryCacheEntry, exception)], edx);
	xMOV(ptr32[r11 + offsetof(microVUSoftUnaryCacheEntry, valid)], 1);
	sqrt_cache_result_ready.SetTarget();
	xMOVAPS(xmm0, ptr128[rsp + saved_xmm]);
	xADD(rsp, stack_size);
	xRET();
}

static void mVUGenerateLowerRsqrtSoftExactKernel(microVU& mVU)
{
	// Internal ABI: eax = raw Fs lane, edx = raw Ft lane. Returns eax = raw
	// result and edx = current I/D exception bits. Normal finite misses keep the
	// exact SQRT correction and DIV quotient in one frame; extended inputs use
	// the separate exact helpers as a cold side exit.
	constexpr sptr fs_raw = 0;
	constexpr int ft_raw = fs_raw + 4;
	constexpr int result_raw = ft_raw + 4;
	constexpr int exception = result_raw + 4;
	constexpr int result_exp = exception + 4;
	constexpr int saved_rbp = result_exp + 4;
	constexpr int saved_rsi = saved_rbp + 8;
	constexpr int saved_xmm = (saved_rsi + 8 + 15) & ~15;
	constexpr int stack_size = saved_xmm + 16 + 8;

	static_assert((stack_size & 15) == 8);
	mVU.softRsqrtExact = xGetAlignedCallTarget();
	xSUB(rsp, stack_size);
	xMOV(ptr32[rsp + fs_raw], eax);
	xMOV(ptr32[rsp + ft_raw], edx);
	xMOV(ptr64[rsp + saved_rbp], rbp);
	xMOV(ptr64[rsp + saved_rsi], rsi);
	xMOVAPS(ptr128[rsp + saved_xmm], xmm0);

	mVUemitLowerSoftBinaryCacheIndex(ecx, eax, edx);
	mVUemitLowerSoftCacheSetAddress(mVU.softRsqrtCache.get());
	xCMP(ptr32[r11 + offsetof(microVUSoftLowerCacheEntry, valid)], 0);
	xForwardJZ32 rsqrt_cache_probe_way1_invalid;
	xCMP(eax, ptr32[r11 + mVUsoftLowerCacheKeyAOffset]);
	xForwardJNE32 rsqrt_cache_probe_way1_a;
	xCMP(edx, ptr32[r11 + offsetof(microVUSoftLowerCacheEntry, key_b)]);
	xForwardJNE32 rsqrt_cache_probe_way1_b;
	xMOV(eax, ptr32[r11 + offsetof(microVUSoftLowerCacheEntry, result)]);
	xMOV(edx, ptr32[r11 + offsetof(microVUSoftLowerCacheEntry, exception)]);
	xForwardJump32 rsqrt_finished_way0;

	rsqrt_cache_probe_way1_invalid.SetTarget();
	rsqrt_cache_probe_way1_a.SetTarget();
	rsqrt_cache_probe_way1_b.SetTarget();
	xADD(r11, sizeof(microVUSoftLowerCacheEntry));
	xCMP(ptr32[r11 + offsetof(microVUSoftLowerCacheEntry, valid)], 0);
	xForwardJZ32 rsqrt_cache_miss_invalid;
	xCMP(eax, ptr32[r11 + mVUsoftLowerCacheKeyAOffset]);
	xForwardJNE32 rsqrt_cache_miss_a;
	xCMP(edx, ptr32[r11 + offsetof(microVUSoftLowerCacheEntry, key_b)]);
	xForwardJNE32 rsqrt_cache_miss_b;
	xMOV(eax, ptr32[r11 + offsetof(microVUSoftLowerCacheEntry, result)]);
	xMOV(edx, ptr32[r11 + offsetof(microVUSoftLowerCacheEntry, exception)]);
	xForwardJump32 rsqrt_finished;

	rsqrt_cache_miss_invalid.SetTarget();
	rsqrt_cache_miss_a.SetTarget();
	rsqrt_cache_miss_b.SetTarget();
	xMOV(ecx, ptr32[rsp + ft_raw]);
	xAND(ecx, 0x7f800000);
	xForwardJZ32 rsqrt_zero_divisor;
	xCMP(ecx, 0x7f800000);
	xForwardJE32 rsqrt_ft_exceptional_side_exit;
	xMOV(ecx, ptr32[rsp + fs_raw]);
	xAND(ecx, 0x7f800000);
	xForwardJZ32 rsqrt_zero_dividend;
	xCMP(ecx, 0x7f800000);
	xForwardJE32 rsqrt_fs_exceptional_side_exit;

	// Start with a chop-mode SQRTSS candidate, then apply the exact PS2 correction.
	xMOVDZX(xmm0, ptr32[rsp + ft_raw]);
	xPAND(xmm0, ptr128[mVUglob.absclip]);
	const bool switch_mxcsr = mVUupperSoftNeedsTruncateMxcsr(mVU);
	if (switch_mxcsr)
		xLDMXCSR(ptr32[&s_vu_soft_truncate_mxcsr]);
	xSQRT.SS(xmm0, xmm0);
	if (switch_mxcsr)
		xLDMXCSR(ptr32[mVU.index == 0 ? &EmuConfig.Cpu.VU0FPCR.bitmask : &EmuConfig.Cpu.VU1FPCR.bitmask]);
	xMOVD(r9d, xmm0);
	xTEST(ptr32[rsp + ft_raw], 0x7fffff);
	xForwardJZ32 rsqrt_sqrt_correction_ready;
	xMOV(eax, r9d);
	xAND(eax, 0x7fffff);
	xOR(eax, 0x800000);
	xMOV(r8d, eax);
	xUMUL(r8);
	// (R + 1)^2 - X > 2^23 is equivalent, over integers, to
	// X <= R^2 + 2R - 2^23. The cap proves the exact SRT floor;
	// unsafe inputs retain the existing correction bitmap.
	xLEA(rax, ptr[r8 * 2 + rax - (1 << 23)]);
	xMOV(r10d, ptr32[rsp + ft_raw]);
	xAND(r10d, 0x7fffff);
	xOR(r10d, 0x800000);
	xSHL(r10, 23);
	xTEST(ptr32[rsp + ft_raw], 0x800000);
	xForwardJNZ8 rsqrt_sqrt_residual_radicand_ready;
	xADD(r10, r10);
	rsqrt_sqrt_residual_radicand_ready.SetTarget();
	xCMP(r10, rax);
	xForwardJBE8 rsqrt_sqrt_residual_floor;
	xMOV(edx, ptr32[rsp + ft_raw]);
	xMOV(ecx, edx);
	xAND(ecx, 0x7fffff);
	xSHR(edx, 23);
	xAND(edx, 1);
	xXOR(edx, 1);
	xSHL(edx, 23);
	xOR(ecx, edx);
	xMOV64(r10, reinterpret_cast<uptr>(MicroVUSoftFloatTables::sqrt_correction_lookup));
	xXOR(eax, eax);
	xBT(ptr32[r10], ecx);
	xSETB(al);
	xADD(eax, r9d);
	xForwardJump8 rsqrt_sqrt_result_ready;
	rsqrt_sqrt_residual_floor.SetTarget();
	xMOV(eax, r9d);
	xForwardJump8 rsqrt_sqrt_result_ready_from_floor;
	rsqrt_sqrt_correction_ready.SetTarget();
	xMOV(eax, r9d);
	rsqrt_sqrt_result_ready_from_floor.SetTarget();
	rsqrt_sqrt_result_ready.SetTarget();
	xMOV(r11d, eax);

	// Feed the exact root directly into the existing VU quotient recurrence.
	xMOV(eax, ptr32[rsp + fs_raw]);
	xSHR(eax, 23);
	xAND(eax, 0xff);
	xMOV(edx, r11d);
	xSHR(edx, 23);
	xAND(edx, 0xff);
	xSUB(eax, edx);
	xADD(eax, 126);
	xMOV(ptr32[rsp + result_exp], eax);
	xCMP(eax, 255);
	xForwardJG32 rsqrt_initial_overflow;
	xCMP(eax, 0);
	xForwardJL32 rsqrt_initial_underflow;

	xMOV(eax, ptr32[rsp + fs_raw]);
	xAND(eax, 0x7fffff);
	xOR(eax, 0x800000);
	xSHL(eax, 2);
	xMOV(r9d, eax);
	xAND(r11d, 0x7fffff);
	xOR(r11d, 0x800000);
	xSHL(r11d, 2);
	xXOR(r10d, r10d);
	xXOR(ebp, ebp);
	xMOV(r8d, 1);
	xMOV(esi, 23);
	u8* const quotient_loop = xGetPtr();
	xSHL(ebp, 1);
	xADD(ebp, r8d);
	X86SoftFloatEmitter::EmitDivCarrySaveStep();
	xDEC(esi);
	xJcc32(Jcc_NotZero,
		static_cast<s32>(reinterpret_cast<sptr>(quotient_loop) - (reinterpret_cast<sptr>(xGetPtr()) + 6)));
	xSHL(ebp, 1);
	xADD(ebp, r8d);
	X86SoftFloatEmitter::EmitDivCarrySaveStep();
	xMOV(eax, ebp);
	xSHL(eax, 1);
	xADD(eax, r8d);
	xCMP(eax, 1 << 24);
	xForwardJL8 rsqrt_quotient_normalized;
	xSHR(eax, 1);
	xINC(ptr32[rsp + result_exp]);
	rsqrt_quotient_normalized.SetTarget();
	xMOV(edx, ptr32[rsp + result_exp]);
	xCMP(edx, 255);
	xForwardJG32 rsqrt_normalized_overflow;
	xCMP(edx, 1);
	xForwardJL32 rsqrt_normalized_underflow;
	xAND(eax, 0x7fffff);
	xSHL(edx, 23);
	xOR(eax, edx);
	xMOV(ecx, ptr32[rsp + fs_raw]);
	xAND(ecx, 0x80000000);
	xOR(eax, ecx);
	xForwardJump32 rsqrt_normal_quotient_result_ready;

	rsqrt_initial_overflow.SetTarget();
	rsqrt_normalized_overflow.SetTarget();
	xMOV(eax, ptr32[rsp + fs_raw]);
	xAND(eax, 0x80000000);
	xOR(eax, 0x7fffffff);
	xForwardJump32 rsqrt_overflow_result_ready;
	rsqrt_initial_underflow.SetTarget();
	rsqrt_normalized_underflow.SetTarget();
	xMOV(eax, ptr32[rsp + fs_raw]);
	xAND(eax, 0x80000000);

	rsqrt_normal_quotient_result_ready.SetTarget();
	rsqrt_overflow_result_ready.SetTarget();
	xXOR(edx, edx);
	xTEST(ptr32[rsp + ft_raw], 0x80000000);
	xForwardJZ32 rsqrt_normal_positive_result_ready;
	xMOV(edx, 0x10);
	xForwardJump32 rsqrt_normal_status_result_ready;

	rsqrt_zero_dividend.SetTarget();
	xMOV(eax, ptr32[rsp + fs_raw]);
	xAND(eax, 0x80000000);
	xXOR(edx, edx);
	xTEST(ptr32[rsp + ft_raw], 0x80000000);
	xForwardJZ32 rsqrt_zero_dividend_positive_result_ready;
	xMOV(edx, 0x10);
	xForwardJump32 rsqrt_zero_dividend_status_result_ready;

	rsqrt_zero_divisor.SetTarget();
	xMOV(eax, 0x7fffffffu);
	xMOV(ecx, ptr32[rsp + fs_raw]);
	xAND(ecx, 0x7fffffff);
	xMOV(edx, 0x10);
	xForwardJZ8 rsqrt_zero_divisor_invalid_result_ready;
	xMOV(edx, 0x20);
	xTEST(ptr32[rsp + ft_raw], 0x80000000);
	xForwardJZ8 rsqrt_zero_divisor_divide_result_ready;
	xOR(edx, 0x10);
	rsqrt_zero_divisor_invalid_result_ready.SetTarget();
	rsqrt_zero_divisor_divide_result_ready.SetTarget();
	xForwardJump32 rsqrt_zero_divisor_result_ready;

	rsqrt_ft_exceptional_side_exit.SetTarget();
	rsqrt_fs_exceptional_side_exit.SetTarget();
	xMOV(eax, ptr32[rsp + ft_raw]);
	xCALL(mVU.softSqrtExact);
	xMOV(edx, eax);
	xMOV(eax, ptr32[rsp + fs_raw]);
	xCALL(mVU.softDivExact);
	xXOR(edx, edx);
	xTEST(ptr32[rsp + ft_raw], 0x80000000);
	xForwardJZ8 rsqrt_exceptional_positive_result_ready;
	xMOV(edx, 0x10);

	rsqrt_normal_positive_result_ready.SetTarget();
	rsqrt_normal_status_result_ready.SetTarget();
	rsqrt_zero_dividend_positive_result_ready.SetTarget();
	rsqrt_zero_dividend_status_result_ready.SetTarget();
	rsqrt_zero_divisor_result_ready.SetTarget();
	rsqrt_exceptional_positive_result_ready.SetTarget();
	xMOV(ptr32[rsp + result_raw], eax);
	xMOV(ptr32[rsp + exception], edx);
	xMOV(eax, ptr32[rsp + fs_raw]);
	xMOV(edx, ptr32[rsp + ft_raw]);
	mVUemitLowerSoftBinaryCacheIndex(ecx, eax, edx);
	mVUemitLowerSoftCacheSetAddress(mVU.softRsqrtCache.get());
	// Way zero's valid word also holds the FIFO victim bit: 1 selects way zero
	// and 3 selects way one on the next full-set refill. Hits never write it.
	xCMP(ptr32[r11 + offsetof(microVUSoftLowerCacheEntry, valid)], 0);
	xForwardJZ8 rsqrt_cache_refill_way0;
	xCMP(ptr32[r11 + sizeof(microVUSoftLowerCacheEntry) +
		offsetof(microVUSoftLowerCacheEntry, valid)], 0);
	xForwardJZ8 rsqrt_cache_refill_way1;
	xTEST(ptr32[r11 + offsetof(microVUSoftLowerCacheEntry, valid)], 2);
	xForwardJNZ8 rsqrt_cache_refill_way1_full;
	xMOV(r10d, 3);
	xForwardJump8 rsqrt_cache_refill_address_ready;
	rsqrt_cache_refill_way0.SetTarget();
	xMOV(r10d, 1);
	xForwardJump8 rsqrt_cache_refill_address_ready_from_way0;
	rsqrt_cache_refill_way1_full.SetTarget();
	xMOV(ptr32[r11 + offsetof(microVUSoftLowerCacheEntry, valid)], 1);
	rsqrt_cache_refill_way1.SetTarget();
	xADD(r11, sizeof(microVUSoftLowerCacheEntry));
	xMOV(r10d, 1);
	rsqrt_cache_refill_address_ready.SetTarget();
	rsqrt_cache_refill_address_ready_from_way0.SetTarget();
	xMOV(ptr32[r11 + mVUsoftLowerCacheKeyAOffset], eax);
	xMOV(ptr32[r11 + offsetof(microVUSoftLowerCacheEntry, key_b)], edx);
	xMOV(eax, ptr32[rsp + result_raw]);
	xMOV(ptr32[r11 + offsetof(microVUSoftLowerCacheEntry, result)], eax);
	xMOV(edx, ptr32[rsp + exception]);
	xMOV(ptr32[r11 + offsetof(microVUSoftLowerCacheEntry, exception)], edx);
	xMOV(ptr32[r11 + offsetof(microVUSoftLowerCacheEntry, valid)], r10d);

	rsqrt_finished_way0.SetTarget();
	rsqrt_finished.SetTarget();
	xMOVAPS(xmm0, ptr128[rsp + saved_xmm]);
	xMOV(rbp, ptr64[rsp + saved_rbp]);
	xMOV(rsi, ptr64[rsp + saved_rsi]);
	xADD(rsp, stack_size);
	xRET();
}

static void mVUemitLowerUnarySoftExact(microVU& mVU, const void* kernel)
{
	mVU.regAlloc->flushCallerSavedGPRs();
	const xmm& ft = mVU.regAlloc->allocReg(_Ft_);
	mVUemitExtractLane(eax, ft, _Ftf_);
	xCALL(kernel);
	mVUemitLowerSoftQAndStatusWriteback(mVU);
	mVU.regAlloc->clearNeeded(ft);
}

static void mVUemitLowerDivSoftExact(microVU& mVU)
{
	if (_Fs_ == 0 && _Fsf_ == 3)
	{
		mVUemitLowerUnarySoftExact(mVU, mVU.softSrtReciprocalExact);
		return;
	}

	mVU.regAlloc->flushCallerSavedGPRs();
	const xmm& fs = mVU.regAlloc->allocReg(_Fs_);
	const xmm& ft = mVU.regAlloc->allocReg(_Ft_);
	mVUemitExtractLane(eax, fs, _Fsf_);
	mVUemitExtractLane(edx, ft, _Ftf_);
	xCALL(mVU.softDivExact);
	mVUemitLowerSoftQAndStatusWriteback(mVU);
	if (ft.Id != fs.Id)
		mVU.regAlloc->clearNeeded(ft);
	mVU.regAlloc->clearNeeded(fs);
}

static void mVUemitLowerRsqrtSoftExact(microVU& mVU)
{
	mVU.regAlloc->flushCallerSavedGPRs();
	const xmm& fs = mVU.regAlloc->allocReg(_Fs_);
	const xmm& ft = mVU.regAlloc->allocReg(_Ft_);
	mVUemitExtractLane(eax, fs, _Fsf_);
	mVUemitExtractLane(edx, ft, _Ftf_);
	xCALL(mVU.softRsqrtExact);
	mVUemitLowerSoftQAndStatusWriteback(mVU);
	if (ft.Id != fs.Id)
		mVU.regAlloc->clearNeeded(ft);
	mVU.regAlloc->clearNeeded(fs);
}

static void mVUemitLowerPSoftExact(microVU& mVU, const void* first_kernel,
	const void* second_kernel = nullptr)
{
	mVU.regAlloc->flushCallerSavedGPRs();
	const xmm& fs = mVU.regAlloc->allocReg(_Fs_);
	mVUemitExtractLane(eax, fs, _Fsf_);
	xCALL(first_kernel);
	if (second_kernel)
		xCALL(second_kernel);
	mVU.regAlloc->clearNeeded(fs);

	xPSHUF.D(xmmPQ, xmmPQ, mVUinfo.writeP ? 0x27 : 0xC6);
	xPINSR.D(xmmPQ, eax, 0);
	xPSHUF.D(xmmPQ, xmmPQ, mVUinfo.writeP ? 0x27 : 0xC6);
}

// Test if Vector is +/- Zero
static __fi void testZero(const xmm& xmmReg, const xmm& xmmTemp, const x32& gprTemp)
{
	xXOR.PS(xmmTemp, xmmTemp);
	xCMPEQ.SS(xmmTemp, xmmReg);
	xPTEST(xmmTemp, xmmTemp);
}

// Test if Vector is Negative (Set Flags and Makes Positive)
static __fi void testNeg(mV, const xmm& xmmReg, const x32& gprTemp)
{
	xMOVMSKPS(gprTemp, xmmReg);
	xTEST(gprTemp, 1);
	xForwardJZ8 skip;
		xMOV(ptr32[&mVU.divFlag], divI);
		xAND.PS(xmmReg, ptr128[mVUglob.absclip]);
	skip.SetTarget();
}

mVUop(mVU_DIV)
{
	pass1
	{
		mVUanalyzeFDIV(mVU, _Fs_, _Fsf_, _Ft_, _Ftf_, 7);
	}
	pass2
	{
		if (CHECK_VU_SOFT(mVU.index))
		{
			mVUemitLowerDivSoftExact(mVU);
			mVU.profiler.EmitOp(opDIV);
			return;
		}

		xmm Ft;
		if (_Ftf_) Ft = mVU.regAlloc->allocReg(_Ft_, 0, (1 << (3 - _Ftf_)));
		else       Ft = mVU.regAlloc->allocReg(_Ft_);
		const xmm& Fs = mVU.regAlloc->allocReg(_Fs_, 0, (1 << (3 - _Fsf_)));
		const xmm& t1 = mVU.regAlloc->allocReg();

		testZero(Ft, t1, gprT1); // Test if Ft is zero
		xForwardJZ8 cjmp; // Skip if not zero

			testZero(Fs, t1, gprT1); // Test if Fs is zero
			xForwardJZ8 ajmp;
				xMOV(ptr32[&mVU.divFlag], divI); // Set invalid flag (0/0)
				xForwardJump8 bjmp;
			ajmp.SetTarget();
				xMOV(ptr32[&mVU.divFlag], divD); // Zero divide (only when not 0/0)
			bjmp.SetTarget();

			xXOR.PS(Fs, Ft);
			xAND.PS(Fs, ptr128[mVUglob.signbit]);
			xOR.PS (Fs, ptr128[mVUglob.maxvals]); // If division by zero, then xmmFs = +/- fmax

			xForwardJump8 djmp;
		cjmp.SetTarget();
			xMOV(ptr32[&mVU.divFlag], 0); // Clear I/D flags
			SSE_DIVSS(mVU, Fs, Ft);
			mVUclamp1(mVU, Fs, t1, 8, true);
		djmp.SetTarget();

		writeQreg(Fs, mVUinfo.writeQ);

		if (mVU.cop2)
		{
			xAND(gprF0, ~0xc0000);
			xOR(gprF0, ptr32[&mVU.divFlag]);
		}

		mVU.regAlloc->clearNeeded(Fs);
		mVU.regAlloc->clearNeeded(Ft);
		mVU.regAlloc->clearNeeded(t1);
		mVU.profiler.EmitOp(opDIV);
	}
	pass3 { mVUlog("DIV Q, vf%02d%s, vf%02d%s", _Fs_, _Fsf_String, _Ft_, _Ftf_String); }
}

mVUop(mVU_SQRT)
{
	pass1
	{
		mVUanalyzeFDIV(mVU, 0, 0, _Ft_, _Ftf_, 7);
	}
	pass2
	{
		if (CHECK_VU_SOFT(mVU.index))
		{
			mVUemitLowerUnarySoftExact(mVU, mVU.softSqrtExact);
			mVU.profiler.EmitOp(opSQRT);
			return;
		}

		const xmm& Ft = mVU.regAlloc->allocReg(_Ft_, 0, (1 << (3 - _Ftf_)));

		xMOV(ptr32[&mVU.divFlag], 0); // Clear I/D flags
		testNeg(mVU, Ft, gprT1); // Check for negative sqrt

		if (CHECK_VU_OVERFLOW(mVU.index)) // Clamp infinities (only need to do positive clamp since xmmFt is positive)
			xMIN.SS(Ft, ptr32[mVUglob.maxvals]);
		xSQRT.SS(Ft, Ft);
		writeQreg(Ft, mVUinfo.writeQ);

		if (mVU.cop2)
		{
			xAND(gprF0, ~0xc0000);
			xOR(gprF0, ptr32[&mVU.divFlag]);
		}

		mVU.regAlloc->clearNeeded(Ft);
		mVU.profiler.EmitOp(opSQRT);
	}
	pass3 { mVUlog("SQRT Q, vf%02d%s", _Ft_, _Ftf_String); }
}

mVUop(mVU_RSQRT)
{
	pass1
	{
		mVUanalyzeFDIV(mVU, _Fs_, _Fsf_, _Ft_, _Ftf_, 13);
	}
	pass2
	{
		if (CHECK_VU_SOFT(mVU.index))
		{
			mVUemitLowerRsqrtSoftExact(mVU);
			mVU.profiler.EmitOp(opRSQRT);
			return;
		}

		const xmm& Fs = mVU.regAlloc->allocReg(_Fs_, 0, (1 << (3 - _Fsf_)));
		const xmm& Ft = mVU.regAlloc->allocReg(_Ft_, 0, (1 << (3 - _Ftf_)));
		const xmm& t1 = mVU.regAlloc->allocReg();

		xMOV(ptr32[&mVU.divFlag], 0); // Clear I/D flags
		testNeg(mVU, Ft, gprT1); // Check for negative sqrt

		xSQRT.SS(Ft, Ft);
		testZero(Ft, t1, gprT1); // Test if Ft is zero
		xForwardJZ8 ajmp; // Skip if not zero

			testZero(Fs, t1, gprT1); // Test if Fs is zero
			xForwardJZ8 bjmp; // Skip if none are
				xMOV(ptr32[&mVU.divFlag], divI); // Set invalid flag (0/0)
				xForwardJump8 cjmp;
			bjmp.SetTarget();
				xMOV(ptr32[&mVU.divFlag], divD); // Zero divide flag (only when not 0/0)
			cjmp.SetTarget();

			xAND.PS(Fs, ptr128[mVUglob.signbit]);
			xOR.PS(Fs, ptr128[mVUglob.maxvals]); // xmmFs = +/-Max

			xForwardJump8 djmp;
		ajmp.SetTarget();
			SSE_DIVSS(mVU, Fs, Ft);
			mVUclamp1(mVU, Fs, t1, 8, true);
		djmp.SetTarget();

		writeQreg(Fs, mVUinfo.writeQ);

		if (mVU.cop2)
		{
			xAND(gprF0, ~0xc0000);
			xOR(gprF0, ptr32[&mVU.divFlag]);
		}

		mVU.regAlloc->clearNeeded(Fs);
		mVU.regAlloc->clearNeeded(Ft);
		mVU.regAlloc->clearNeeded(t1);
		mVU.profiler.EmitOp(opRSQRT);
	}
	pass3 { mVUlog("RSQRT Q, vf%02d%s, vf%02d%s", _Fs_, _Fsf_String, _Ft_, _Ftf_String); }
}

//------------------------------------------------------------------
// EATAN/EEXP/ELENG/ERCPR/ERLENG/ERSADD/ERSQRT/ESADD/ESIN/ESQRT/ESUM
//------------------------------------------------------------------

#define EATANhelper(addr) \
	{ \
		SSE_MULSS(mVU, t2, Fs); \
		SSE_MULSS(mVU, t2, Fs); \
		xMOVAPS(t1, t2); \
		xMUL.SS(t1, ptr32[addr]); \
		SSE_ADDSS(mVU, PQ, t1); \
	}

// ToDo: Can Be Optimized Further? (takes approximately (~115 cycles + mem access time) on a c2d)
static __fi void mVU_EATAN_(mV, const xmm& PQ, const xmm& Fs, const xmm& t1, const xmm& t2)
{
	xMOVSS(PQ, Fs);
	xMUL.SS(PQ, ptr32[mVUglob.T1]);
	xMOVAPS(t2, Fs);
	EATANhelper(mVUglob.T2);
	EATANhelper(mVUglob.T3);
	EATANhelper(mVUglob.T4);
	EATANhelper(mVUglob.T5);
	EATANhelper(mVUglob.T6);
	EATANhelper(mVUglob.T7);
	EATANhelper(mVUglob.T8);
	xADD.SS(PQ, ptr32[mVUglob.Pi4]);
	xPSHUF.D(PQ, PQ, mVUinfo.writeP ? 0x27 : 0xC6);
}

mVUop(mVU_EATAN)
{
	pass1
	{
		if (isVU0)
		{
			mVUlow.isNOP = true;
			return;
		}
		mVUanalyzeEFU1(mVU, _Fs_, _Fsf_, 54);
	}
	pass2
	{
		const xmm& Fs = mVU.regAlloc->allocReg(_Fs_, 0, (1 << (3 - _Fsf_)));
		const xmm& t1 = mVU.regAlloc->allocReg();
		const xmm& t2 = mVU.regAlloc->allocReg();
		xPSHUF.D(xmmPQ, xmmPQ, mVUinfo.writeP ? 0x27 : 0xC6); // Flip xmmPQ to get Valid P instance
		xMOVSS (xmmPQ, Fs);
		xSUB.SS(Fs,    ptr32[mVUglob.one]);
		xADD.SS(xmmPQ, ptr32[mVUglob.one]);
		SSE_DIVSS(mVU, Fs, xmmPQ);
		mVU_EATAN_(mVU, xmmPQ, Fs, t1, t2);
		mVU.regAlloc->clearNeeded(Fs);
		mVU.regAlloc->clearNeeded(t1);
		mVU.regAlloc->clearNeeded(t2);
		mVU.profiler.EmitOp(opEATAN);
	}
	pass3 { mVUlog("EATAN P"); }
}

mVUop(mVU_EATANxy)
{
	pass1
	{
		if (isVU0)
		{
			mVUlow.isNOP = true;
			return;
		}
		mVUanalyzeEFU2(mVU, _Fs_, 54);
	}
	pass2
	{
		const xmm& t1 = mVU.regAlloc->allocReg(_Fs_, 0, 0xf);
		const xmm& Fs = mVU.regAlloc->allocReg();
		const xmm& t2 = mVU.regAlloc->allocReg();
		xPSHUF.D(Fs, t1, 0x01);
		xPSHUF.D(xmmPQ, xmmPQ, mVUinfo.writeP ? 0x27 : 0xC6); // Flip xmmPQ to get Valid P instance
		xMOVSS  (xmmPQ, Fs);
		SSE_SUBSS (mVU, Fs, t1); // y-x, not y-1? ><
		SSE_ADDSS (mVU, t1, xmmPQ);
		SSE_DIVSS (mVU, Fs, t1);
		mVU_EATAN_(mVU, xmmPQ, Fs, t1, t2);
		mVU.regAlloc->clearNeeded(Fs);
		mVU.regAlloc->clearNeeded(t1);
		mVU.regAlloc->clearNeeded(t2);
		mVU.profiler.EmitOp(opEATANxy);
	}
	pass3 { mVUlog("EATANxy P"); }
}

mVUop(mVU_EATANxz)
{
	pass1
	{
		if (isVU0)
		{
			mVUlow.isNOP = true;
			return;
		}
		mVUanalyzeEFU2(mVU, _Fs_, 54);
	}
	pass2
	{
		const xmm& t1 = mVU.regAlloc->allocReg(_Fs_, 0, 0xf);
		const xmm& Fs = mVU.regAlloc->allocReg();
		const xmm& t2 = mVU.regAlloc->allocReg();
		xPSHUF.D(Fs, t1, 0x02);
		xPSHUF.D(xmmPQ, xmmPQ, mVUinfo.writeP ? 0x27 : 0xC6); // Flip xmmPQ to get Valid P instance
		xMOVSS  (xmmPQ, Fs);
		SSE_SUBSS (mVU, Fs, t1);
		SSE_ADDSS (mVU, t1, xmmPQ);
		SSE_DIVSS (mVU, Fs, t1);
		mVU_EATAN_(mVU, xmmPQ, Fs, t1, t2);
		mVU.regAlloc->clearNeeded(Fs);
		mVU.regAlloc->clearNeeded(t1);
		mVU.regAlloc->clearNeeded(t2);
		mVU.profiler.EmitOp(opEATANxz);
	}
	pass3 { mVUlog("EATANxz P"); }
}

#define eexpHelper(addr) \
	{ \
		SSE_MULSS(mVU, t2, Fs); \
		xMOVAPS(t1, t2); \
		xMUL.SS(t1, ptr32[addr]); \
		SSE_ADDSS(mVU, xmmPQ, t1); \
	}

mVUop(mVU_EEXP)
{
	pass1
	{
		if (isVU0)
		{
			mVUlow.isNOP = true;
			return;
		}
		mVUanalyzeEFU1(mVU, _Fs_, _Fsf_, 44);
	}
	pass2
	{
		const xmm& Fs = mVU.regAlloc->allocReg(_Fs_, 0, (1 << (3 - _Fsf_)));
		const xmm& t1 = mVU.regAlloc->allocReg();
		const xmm& t2 = mVU.regAlloc->allocReg();
		xPSHUF.D(xmmPQ, xmmPQ, mVUinfo.writeP ? 0x27 : 0xC6); // Flip xmmPQ to get Valid P instance
		xMOVSS  (xmmPQ, Fs);
		xMUL.SS (xmmPQ, ptr32[mVUglob.E1]);
		xADD.SS (xmmPQ, ptr32[mVUglob.one]);
		xMOVAPS(t1, Fs);
		SSE_MULSS(mVU, t1, Fs);
		xMOVAPS(t2, t1);
		xMUL.SS(t1, ptr32[mVUglob.E2]);
		SSE_ADDSS(mVU, xmmPQ, t1);
		eexpHelper(&mVUglob.E3);
		eexpHelper(&mVUglob.E4);
		eexpHelper(&mVUglob.E5);
		SSE_MULSS(mVU, t2, Fs);
		xMUL.SS(t2, ptr32[mVUglob.E6]);
		SSE_ADDSS(mVU, xmmPQ, t2);
		SSE_MULSS(mVU, xmmPQ, xmmPQ);
		SSE_MULSS(mVU, xmmPQ, xmmPQ);
		xMOVSSZX(t2, ptr32[mVUglob.one]);
		SSE_DIVSS(mVU, t2, xmmPQ);
		xMOVSS(xmmPQ, t2);
		xPSHUF.D(xmmPQ, xmmPQ, mVUinfo.writeP ? 0x27 : 0xC6); // Flip back
		mVU.regAlloc->clearNeeded(Fs);
		mVU.regAlloc->clearNeeded(t1);
		mVU.regAlloc->clearNeeded(t2);
		mVU.profiler.EmitOp(opEEXP);
	}
	pass3 { mVUlog("EEXP P"); }
}

// sumXYZ(): PQ.x = x ^ 2 + y ^ 2 + z ^ 2
static __fi void mVU_sumXYZ(mV, const xmm& PQ, const xmm& Fs)
{
	xDP.PS(Fs, Fs, 0x71);
	xMOVSS(PQ, Fs);
}

mVUop(mVU_ELENG)
{
	pass1
	{
		if (isVU0)
		{
			mVUlow.isNOP = true;
			return;
		}
		mVUanalyzeEFU2(mVU, _Fs_, 18);
	}
	pass2
	{
		const xmm& Fs = mVU.regAlloc->allocReg(_Fs_, 0, _X_Y_Z_W);
		xPSHUF.D       (xmmPQ, xmmPQ, mVUinfo.writeP ? 0x27 : 0xC6); // Flip xmmPQ to get Valid P instance
		mVU_sumXYZ(mVU, xmmPQ, Fs);
		xSQRT.SS       (xmmPQ, xmmPQ);
		xPSHUF.D       (xmmPQ, xmmPQ, mVUinfo.writeP ? 0x27 : 0xC6); // Flip back
		mVU.regAlloc->clearNeeded(Fs);
		mVU.profiler.EmitOp(opELENG);
	}
	pass3 { mVUlog("ELENG P"); }
}

mVUop(mVU_ERCPR)
{
	pass1
	{
		if (isVU0)
		{
			mVUlow.isNOP = true;
			return;
		}
		mVUanalyzeEFU1(mVU, _Fs_, _Fsf_, 12);
	}
	pass2
	{
		if (CHECK_VU_SOFT(1))
		{
			mVUemitLowerPSoftExact(mVU, mVU.softSrtReciprocalExact);
		}
		else
		{
			const xmm& Fs = mVU.regAlloc->allocReg(_Fs_, 0, (1 << (3 - _Fsf_)));
			xPSHUF.D      (xmmPQ, xmmPQ, mVUinfo.writeP ? 0x27 : 0xC6); // Flip xmmPQ to get Valid P instance
			xMOVSS        (xmmPQ, Fs);
			xMOVSSZX      (Fs, ptr32[mVUglob.one]);
			SSE_DIVSS(mVU, Fs, xmmPQ);
			xMOVSS        (xmmPQ, Fs);
			xPSHUF.D      (xmmPQ, xmmPQ, mVUinfo.writeP ? 0x27 : 0xC6); // Flip back
			mVU.regAlloc->clearNeeded(Fs);
		}
		mVU.profiler.EmitOp(opERCPR);
	}
	pass3 { mVUlog("ERCPR P"); }
}

mVUop(mVU_ERLENG)
{
	pass1
	{
		if (isVU0)
		{
			mVUlow.isNOP = true;
			return;
		}
		mVUanalyzeEFU2(mVU, _Fs_, 24);
	}
	pass2
	{
		const xmm& Fs = mVU.regAlloc->allocReg(_Fs_, 0, _X_Y_Z_W);
		xPSHUF.D       (xmmPQ, xmmPQ, mVUinfo.writeP ? 0x27 : 0xC6); // Flip xmmPQ to get Valid P instance
		mVU_sumXYZ(mVU, xmmPQ, Fs);
		xSQRT.SS       (xmmPQ, xmmPQ);
		xMOVSSZX       (Fs, ptr32[mVUglob.one]);
		SSE_DIVSS (mVU, Fs, xmmPQ);
		xMOVSS         (xmmPQ, Fs);
		xPSHUF.D       (xmmPQ, xmmPQ, mVUinfo.writeP ? 0x27 : 0xC6); // Flip back
		mVU.regAlloc->clearNeeded(Fs);
		mVU.profiler.EmitOp(opERLENG);
	}
	pass3 { mVUlog("ERLENG P"); }
}

mVUop(mVU_ERSADD)
{
	pass1
	{
		if (isVU0)
		{
			mVUlow.isNOP = true;
			return;
		}
		mVUanalyzeEFU2(mVU, _Fs_, 18);
	}
	pass2
	{
		const xmm& Fs = mVU.regAlloc->allocReg(_Fs_, 0, _X_Y_Z_W);
		xPSHUF.D       (xmmPQ, xmmPQ, mVUinfo.writeP ? 0x27 : 0xC6); // Flip xmmPQ to get Valid P instance
		mVU_sumXYZ(mVU, xmmPQ, Fs);
		xMOVSSZX       (Fs, ptr32[mVUglob.one]);
		SSE_DIVSS (mVU, Fs, xmmPQ);
		xMOVSS         (xmmPQ, Fs);
		xPSHUF.D       (xmmPQ, xmmPQ, mVUinfo.writeP ? 0x27 : 0xC6); // Flip back
		mVU.regAlloc->clearNeeded(Fs);
		mVU.profiler.EmitOp(opERSADD);
	}
	pass3 { mVUlog("ERSADD P"); }
}

mVUop(mVU_ERSQRT)
{
	pass1
	{
		if (isVU0)
		{
			mVUlow.isNOP = true;
			return;
		}
		mVUanalyzeEFU1(mVU, _Fs_, _Fsf_, 18);
	}
	pass2
	{
		if (CHECK_VU_SOFT(1))
		{
			mVUemitLowerPSoftExact(mVU, mVU.softSqrtExact, mVU.softSrtReciprocalExact);
		}
		else
		{
			const xmm& Fs = mVU.regAlloc->allocReg(_Fs_, 0, (1 << (3 - _Fsf_)));
			xPSHUF.D      (xmmPQ, xmmPQ, mVUinfo.writeP ? 0x27 : 0xC6); // Flip xmmPQ to get Valid P instance
			xAND.PS       (Fs, ptr128[mVUglob.absclip]);
			xSQRT.SS      (xmmPQ, Fs);
			xMOVSSZX      (Fs, ptr32[mVUglob.one]);
			SSE_DIVSS(mVU, Fs, xmmPQ);
			xMOVSS        (xmmPQ, Fs);
			xPSHUF.D      (xmmPQ, xmmPQ, mVUinfo.writeP ? 0x27 : 0xC6); // Flip back
			mVU.regAlloc->clearNeeded(Fs);
		}
		mVU.profiler.EmitOp(opERSQRT);
	}
	pass3 { mVUlog("ERSQRT P"); }
}

mVUop(mVU_ESADD)
{
	pass1
	{
		if (isVU0)
		{
			mVUlow.isNOP = true;
			return;
		}
		mVUanalyzeEFU2(mVU, _Fs_, 11);
	}
	pass2
	{
		const xmm& Fs = mVU.regAlloc->allocReg(_Fs_, 0, _X_Y_Z_W);
		xPSHUF.D(xmmPQ, xmmPQ, mVUinfo.writeP ? 0x27 : 0xC6); // Flip xmmPQ to get Valid P instance
		mVU_sumXYZ(mVU, xmmPQ, Fs);
		xPSHUF.D(xmmPQ, xmmPQ, mVUinfo.writeP ? 0x27 : 0xC6); // Flip back
		mVU.regAlloc->clearNeeded(Fs);
		mVU.profiler.EmitOp(opESADD);
	}
	pass3 { mVUlog("ESADD P"); }
}

mVUop(mVU_ESIN)
{
	pass1
	{
		if (isVU0)
		{
			mVUlow.isNOP = true;
			return;
		}
		mVUanalyzeEFU1(mVU, _Fs_, _Fsf_, 29);
	}
	pass2
	{
		const xmm& Fs = mVU.regAlloc->allocReg(_Fs_, 0, (1 << (3 - _Fsf_)));
		const xmm& t1 = mVU.regAlloc->allocReg();
		const xmm& t2 = mVU.regAlloc->allocReg();
		xPSHUF.D      (xmmPQ, xmmPQ, mVUinfo.writeP ? 0x27 : 0xC6); // Flip xmmPQ to get Valid P instance
		xMOVSS        (xmmPQ, Fs); // pq = X
		SSE_MULSS(mVU, Fs, Fs);    // fs = X^2
		xMOVAPS       (t1, Fs);    // t1 = X^2
		SSE_MULSS(mVU, Fs, xmmPQ); // fs = X^3
		xMOVAPS       (t2, Fs);    // t2 = X^3
		xMUL.SS       (Fs, ptr32[mVUglob.S2]); // fs = s2 * X^3
		SSE_ADDSS(mVU, xmmPQ, Fs); // pq = X + s2 * X^3

		SSE_MULSS(mVU, t2, t1);    // t2 = X^3 * X^2
		xMUL.SS       (Fs, t2, ptr32[mVUglob.S3]); // ps = s3 * X^5
		SSE_ADDSS(mVU, xmmPQ, Fs); // pq = X + s2 * X^3 + s3 * X^5

		SSE_MULSS(mVU, t2, t1);    // t2 = X^5 * X^2
		xMUL.SS       (Fs, t2, ptr32[mVUglob.S4]); // fs = s4 * X^7
		SSE_ADDSS(mVU, xmmPQ, Fs); // pq = X + s2 * X^3 + s3 * X^5 + s4 * X^7

		SSE_MULSS(mVU, t2, t1);    // t2 = X^7 * X^2
		xMUL.SS       (t2, ptr32[mVUglob.S5]); // t2 = s5 * X^9
		SSE_ADDSS(mVU, xmmPQ, t2); // pq = X + s2 * X^3 + s3 * X^5 + s4 * X^7 + s5 * X^9
		xPSHUF.D      (xmmPQ, xmmPQ, mVUinfo.writeP ? 0x27 : 0xC6); // Flip back
		mVU.regAlloc->clearNeeded(Fs);
		mVU.regAlloc->clearNeeded(t1);
		mVU.regAlloc->clearNeeded(t2);
		mVU.profiler.EmitOp(opESIN);
	}
	pass3 { mVUlog("ESIN P"); }
}

mVUop(mVU_ESQRT)
{
	pass1
	{
		if (isVU0)
		{
			mVUlow.isNOP = true;
			return;
		}
		mVUanalyzeEFU1(mVU, _Fs_, _Fsf_, 12);
	}
	pass2
	{
		if (CHECK_VU_SOFT(1))
		{
			mVUemitLowerPSoftExact(mVU, mVU.softSqrtExact);
		}
		else
		{
			const xmm& Fs = mVU.regAlloc->allocReg(_Fs_, 0, (1 << (3 - _Fsf_)));
			xPSHUF.D(xmmPQ, xmmPQ, mVUinfo.writeP ? 0x27 : 0xC6); // Flip xmmPQ to get Valid P instance
			xAND.PS (Fs, ptr128[mVUglob.absclip]);
			xSQRT.SS(xmmPQ, Fs);
			xPSHUF.D(xmmPQ, xmmPQ, mVUinfo.writeP ? 0x27 : 0xC6); // Flip back
			mVU.regAlloc->clearNeeded(Fs);
		}
		mVU.profiler.EmitOp(opESQRT);
	}
	pass3 { mVUlog("ESQRT P"); }
}

mVUop(mVU_ESUM)
{
	pass1
	{
		if (isVU0)
		{
			mVUlow.isNOP = true;
			return;
		}
		mVUanalyzeEFU2(mVU, _Fs_, 12);
	}
	pass2
	{
		const xmm& Fs = mVU.regAlloc->allocReg(_Fs_, 0, _X_Y_Z_W);
		const xmm& t1 = mVU.regAlloc->allocReg();
		xPSHUF.D(xmmPQ, xmmPQ, mVUinfo.writeP ? 0x27 : 0xC6); // Flip xmmPQ to get Valid P instance
		xPSHUF.D(t1, Fs, 0x1b);
		SSE_ADDPS(mVU, Fs, t1);
		xPSHUF.D(t1, Fs, 0x01);
		SSE_ADDSS(mVU, Fs, t1);
		xMOVSS(xmmPQ, Fs);
		xPSHUF.D(xmmPQ, xmmPQ, mVUinfo.writeP ? 0x27 : 0xC6); // Flip back
		mVU.regAlloc->clearNeeded(Fs);
		mVU.regAlloc->clearNeeded(t1);
		mVU.profiler.EmitOp(opESUM);
	}
	pass3 { mVUlog("ESUM P"); }
}

//------------------------------------------------------------------
// FCAND/FCEQ/FCGET/FCOR/FCSET
//------------------------------------------------------------------

mVUop(mVU_FCAND)
{
	pass1 { mVUanalyzeCflag(mVU, 1); }
	pass2
	{
		const xRegister32& dst = mVU.regAlloc->allocGPR(-1, 1, mVUlow.backupVI);
		mVUallocCFLAGa(mVU, dst, cFLAG.read);
		xAND(dst, _Imm24_);
		xADD(dst, 0xffffff);
		xSHR(dst, 24);
		mVU.regAlloc->clearNeeded(dst);
		mVU.profiler.EmitOp(opFCAND);
	}
	pass3 { mVUlog("FCAND vi01, $%x", _Imm24_); }
	pass4 { mVUregs.needExactMatch |= 4; }
}

mVUop(mVU_FCEQ)
{
	pass1 { mVUanalyzeCflag(mVU, 1); }
	pass2
	{
		const xRegister32& dst = mVU.regAlloc->allocGPR(-1, 1, mVUlow.backupVI);
		mVUallocCFLAGa(mVU, dst, cFLAG.read);
		xXOR(dst, _Imm24_);
		xSUB(dst, 1);
		xSHR(dst, 31);
		mVU.regAlloc->clearNeeded(dst);
		mVU.profiler.EmitOp(opFCEQ);
	}
	pass3 { mVUlog("FCEQ vi01, $%x", _Imm24_); }
	pass4 { mVUregs.needExactMatch |= 4; }
}

mVUop(mVU_FCGET)
{
	pass1 { mVUanalyzeCflag(mVU, _It_); }
	pass2
	{
		const xRegister32& regT = mVU.regAlloc->allocGPR(-1, _It_, mVUlow.backupVI);
		mVUallocCFLAGa(mVU, regT, cFLAG.read);
		xAND(regT, 0xfff);
		mVU.regAlloc->clearNeeded(regT);
		mVU.profiler.EmitOp(opFCGET);
	}
	pass3 { mVUlog("FCGET vi%02d", _Ft_); }
	pass4 { mVUregs.needExactMatch |= 4; }
}

mVUop(mVU_FCOR)
{
	pass1 { mVUanalyzeCflag(mVU, 1); }
	pass2
	{
		const xRegister32& dst = mVU.regAlloc->allocGPR(-1, 1, mVUlow.backupVI);
		mVUallocCFLAGa(mVU, dst, cFLAG.read);
		xOR(dst, _Imm24_);
		xADD(dst, 1);  // If 24 1's will make 25th bit 1, else 0
		xSHR(dst, 24); // Get the 25th bit (also clears the rest of the garbage in the reg)
		mVU.regAlloc->clearNeeded(dst);
		mVU.profiler.EmitOp(opFCOR);
	}
	pass3 { mVUlog("FCOR vi01, $%x", _Imm24_); }
	pass4 { mVUregs.needExactMatch |= 4; }
}

mVUop(mVU_FCSET)
{
	pass1 { cFLAG.doFlag = true; }
	pass2
	{
		xMOV(gprT1, _Imm24_);
		mVUallocCFLAGb(mVU, gprT1, cFLAG.write);
		mVU.profiler.EmitOp(opFCSET);
	}
	pass3 { mVUlog("FCSET $%x", _Imm24_); }
}

//------------------------------------------------------------------
// FMAND/FMEQ/FMOR
//------------------------------------------------------------------

mVUop(mVU_FMAND)
{
	pass1 { mVUanalyzeMflag(mVU, _Is_, _It_); }
	pass2
	{
		mVUallocMFLAGa(mVU, gprT1, mFLAG.read);
		const xRegister32& regT = mVU.regAlloc->allocGPR(_Is_, _It_, mVUlow.backupVI);
		xAND(regT, gprT1);
		mVU.regAlloc->clearNeeded(regT);
		mVU.profiler.EmitOp(opFMAND);
	}
	pass3 { mVUlog("FMAND vi%02d, vi%02d", _Ft_, _Fs_); }
	pass4 { mVUregs.needExactMatch |= 2; }
}

mVUop(mVU_FMEQ)
{
	pass1 { mVUanalyzeMflag(mVU, _Is_, _It_); }
	pass2
	{
		mVUallocMFLAGa(mVU, gprT1, mFLAG.read);
		const xRegister32& regT = mVU.regAlloc->allocGPR(_Is_, _It_, mVUlow.backupVI);
		xXOR(regT, gprT1);
		xSUB(regT, 1);
		xSHR(regT, 31);
		mVU.regAlloc->clearNeeded(regT);
		mVU.profiler.EmitOp(opFMEQ);
	}
	pass3 { mVUlog("FMEQ vi%02d, vi%02d", _Ft_, _Fs_); }
	pass4 { mVUregs.needExactMatch |= 2; }
}

mVUop(mVU_FMOR)
{
	pass1 { mVUanalyzeMflag(mVU, _Is_, _It_); }
	pass2
	{
		mVUallocMFLAGa(mVU, gprT1, mFLAG.read);
		const xRegister32& regT = mVU.regAlloc->allocGPR(_Is_, _It_, mVUlow.backupVI);
		xOR(regT, gprT1);
		mVU.regAlloc->clearNeeded(regT);
		mVU.profiler.EmitOp(opFMOR);
	}
	pass3 { mVUlog("FMOR vi%02d, vi%02d", _Ft_, _Fs_); }
	pass4 { mVUregs.needExactMatch |= 2; }
}

//------------------------------------------------------------------
// FSAND/FSEQ/FSOR/FSSET
//------------------------------------------------------------------

mVUop(mVU_FSAND)
{
	pass1 { mVUanalyzeSflag(mVU, _It_); }
	pass2
	{
		if (_Imm12_ & 0x0c30) DevCon.WriteLn(Color_Green, "mVU_FSAND: Checking I/D/IS/DS Flags");
		if (_Imm12_ & 0x030c) DevCon.WriteLn(Color_Green, "mVU_FSAND: Checking U/O/US/OS Flags");
		const xRegister32& reg = mVU.regAlloc->allocGPR(-1, _It_, mVUlow.backupVI);
		mVUallocSFLAGc(reg, gprT1, sFLAG.read);
		xAND(reg, _Imm12_);
		mVU.regAlloc->clearNeeded(reg);
		mVU.profiler.EmitOp(opFSAND);
	}
	pass3 { mVUlog("FSAND vi%02d, $%x", _Ft_, _Imm12_); }
	pass4 { mVUregs.needExactMatch |= 1; }
}

mVUop(mVU_FSOR)
{
	pass1 { mVUanalyzeSflag(mVU, _It_); }
	pass2
	{
		const xRegister32& reg = mVU.regAlloc->allocGPR(-1, _It_, mVUlow.backupVI);
		mVUallocSFLAGc(reg, gprT2, sFLAG.read);
		xOR(reg, _Imm12_);
		mVU.regAlloc->clearNeeded(reg);
		mVU.profiler.EmitOp(opFSOR);
	}
	pass3 { mVUlog("FSOR vi%02d, $%x", _Ft_, _Imm12_); }
	pass4 { mVUregs.needExactMatch |= 1; }
}

mVUop(mVU_FSEQ)
{
	pass1 { mVUanalyzeSflag(mVU, _It_); }
	pass2
	{
		int imm = 0;
		if (_Imm12_ & 0x0c30) DevCon.WriteLn(Color_Green, "mVU_FSEQ: Checking I/D/IS/DS Flags");
		if (_Imm12_ & 0x030c) DevCon.WriteLn(Color_Green, "mVU_FSEQ: Checking U/O/US/OS Flags");
		if (_Imm12_ & 0x0001) imm |= 0x0000f00; // Z
		if (_Imm12_ & 0x0002) imm |= 0x000f000; // S
		if (_Imm12_ & 0x0004) imm |= 0x0010000; // U
		if (_Imm12_ & 0x0008) imm |= 0x0020000; // O
		if (_Imm12_ & 0x0010) imm |= 0x0040000; // I
		if (_Imm12_ & 0x0020) imm |= 0x0080000; // D
		if (_Imm12_ & 0x0040) imm |= 0x000000f; // ZS
		if (_Imm12_ & 0x0080) imm |= 0x00000f0; // SS
		if (_Imm12_ & 0x0100) imm |= 0x0400000; // US
		if (_Imm12_ & 0x0200) imm |= 0x0800000; // OS
		if (_Imm12_ & 0x0400) imm |= 0x1000000; // IS
		if (_Imm12_ & 0x0800) imm |= 0x2000000; // DS

		const xRegister32& reg = mVU.regAlloc->allocGPR(-1, _It_, mVUlow.backupVI);
		mVUallocSFLAGa(reg, sFLAG.read);
		setBitFSEQ(reg, 0x0f00); // Z  bit
		setBitFSEQ(reg, 0xf000); // S  bit
		setBitFSEQ(reg, 0x000f); // ZS bit
		setBitFSEQ(reg, 0x00f0); // SS bit
		xXOR(reg, imm);
		xSUB(reg, 1);
		xSHR(reg, 31);
		mVU.regAlloc->clearNeeded(reg);
		mVU.profiler.EmitOp(opFSEQ);
	}
	pass3 { mVUlog("FSEQ vi%02d, $%x", _Ft_, _Imm12_); }
	pass4 { mVUregs.needExactMatch |= 1; }
}

mVUop(mVU_FSSET)
{
	pass1 { mVUanalyzeFSSET(mVU); }
	pass2
	{
		int imm = 0;
		if (_Imm12_ & 0x0040) imm |= 0x000000f; // ZS
		if (_Imm12_ & 0x0080) imm |= 0x00000f0; // SS
		if (_Imm12_ & 0x0100) imm |= 0x0400000; // US
		if (_Imm12_ & 0x0200) imm |= 0x0800000; // OS
		if (_Imm12_ & 0x0400) imm |= 0x1000000; // IS
		if (_Imm12_ & 0x0800) imm |= 0x2000000; // DS
		if (!(sFLAG.doFlag || mVUinfo.doDivFlag))
		{
			mVUallocSFLAGa(getFlagReg(sFLAG.write), sFLAG.lastWrite); // Get Prev Status Flag
		}
		xAND(getFlagReg(sFLAG.write), 0xfff00); // Keep Non-Sticky Bits
		if (imm)
			xOR(getFlagReg(sFLAG.write), imm);
		mVU.profiler.EmitOp(opFSSET);
	}
	pass3 { mVUlog("FSSET $%x", _Imm12_); }
}

//------------------------------------------------------------------
// IADD/IADDI/IADDIU/IAND/IOR/ISUB/ISUBIU
//------------------------------------------------------------------

mVUop(mVU_IADD)
{
	pass1 { mVUanalyzeIALU1(mVU, _Id_, _Is_, _It_); }
	pass2
	{
		if (_Is_ == 0 || _It_ == 0)
		{
			const xRegister32& regS = mVU.regAlloc->allocGPR(_Is_ ? _Is_ : _It_, -1);
			const xRegister32& regD = mVU.regAlloc->allocGPR(-1, _Id_, mVUlow.backupVI);
			xMOV(regD, regS);
			mVU.regAlloc->clearNeeded(regD);
			mVU.regAlloc->clearNeeded(regS);
		}
		else
		{
			const xRegister32& regT = mVU.regAlloc->allocGPR(_It_, -1);
			const xRegister32& regS = mVU.regAlloc->allocGPR(_Is_, _Id_, mVUlow.backupVI);
			xADD(regS, regT);
			mVU.regAlloc->clearNeeded(regS);
			mVU.regAlloc->clearNeeded(regT);
		}
		mVU.profiler.EmitOp(opIADD);
	}
	pass3 { mVUlog("IADD vi%02d, vi%02d, vi%02d", _Fd_, _Fs_, _Ft_); }
}

mVUop(mVU_IADDI)
{
	pass1 { mVUanalyzeIADDI(mVU, _Is_, _It_, _Imm5_); }
	pass2
	{
		if (_Is_ == 0)
		{
			const xRegister32& regT = mVU.regAlloc->allocGPR(-1, _It_, mVUlow.backupVI);
			if (!EmuConfig.Gamefixes.IbitHack)
			{
				if (_Imm5_ != 0)
					xMOV(regT, _Imm5_);
				else
					xXOR(regT, regT);
			}
			else
			{
				xMOV(regT, ptr32[&curI]);
				xSHL(regT, 21);
				xSAR(regT, 27);
			}
			mVU.regAlloc->clearNeeded(regT);
		}
		else
		{
			const xRegister32& regS = mVU.regAlloc->allocGPR(_Is_, _It_, mVUlow.backupVI);
			if (!EmuConfig.Gamefixes.IbitHack)
			{
				if (_Imm5_ != 0)
					xADD(regS, _Imm5_);
			}
			else
			{
				xMOV(gprT1, ptr32[&curI]);
				xSHL(gprT1, 21);
				xSAR(gprT1, 27);

				xADD(regS, gprT1);
			}
			mVU.regAlloc->clearNeeded(regS);
		}
		mVU.profiler.EmitOp(opIADDI);
	}
	pass3 { mVUlog("IADDI vi%02d, vi%02d, %d", _Ft_, _Fs_, _Imm5_); }
}

mVUop(mVU_IADDIU)
{
	pass1 { mVUanalyzeIADDI(mVU, _Is_, _It_, _Imm15_); }
	pass2
	{
		if (_Is_ == 0)
		{
			const xRegister32& regT = mVU.regAlloc->allocGPR(-1, _It_, mVUlow.backupVI);
			if (!EmuConfig.Gamefixes.IbitHack)
			{
				if (_Imm15_ != 0)
					xMOV(regT, _Imm15_);
				else
					xXOR(regT, regT);
			}
			else
			{
				xMOV(regT, ptr32[&curI]);
				xMOV(gprT1, regT);
				xSHR(gprT1, 10);
				xAND(gprT1, 0x7800);
				xAND(regT, 0x7FF);
				xOR(regT, gprT1);
			}
			mVU.regAlloc->clearNeeded(regT);
		}
		else
		{
			const xRegister32& regS = mVU.regAlloc->allocGPR(_Is_, _It_, mVUlow.backupVI);
			if (!EmuConfig.Gamefixes.IbitHack)
			{
				if (_Imm15_ != 0)
					xADD(regS, _Imm15_);
			}
			else
			{
				xMOV(gprT1, ptr32[&curI]);
				xMOV(gprT2, gprT1);
				xSHR(gprT2, 10);
				xAND(gprT2, 0x7800);
				xAND(gprT1, 0x7FF);
				xOR(gprT1, gprT2);

				xADD(regS, gprT1);
			}
			mVU.regAlloc->clearNeeded(regS);
		}
		mVU.profiler.EmitOp(opIADDIU);
	}
	pass3 { mVUlog("IADDIU vi%02d, vi%02d, %d", _Ft_, _Fs_, _Imm15_); }
}

mVUop(mVU_IAND)
{
	pass1 { mVUanalyzeIALU1(mVU, _Id_, _Is_, _It_); }
	pass2
	{
		const xRegister32& regT = mVU.regAlloc->allocGPR(_It_, -1);
		const xRegister32& regS = mVU.regAlloc->allocGPR(_Is_, _Id_, mVUlow.backupVI);
		if (_It_ != _Is_)
			xAND(regS, regT);
		mVU.regAlloc->clearNeeded(regS);
		mVU.regAlloc->clearNeeded(regT);
		mVU.profiler.EmitOp(opIAND);
	}
	pass3 { mVUlog("IAND vi%02d, vi%02d, vi%02d", _Fd_, _Fs_, _Ft_); }
}

mVUop(mVU_IOR)
{
	pass1 { mVUanalyzeIALU1(mVU, _Id_, _Is_, _It_); }
	pass2
	{
		const xRegister32& regT = mVU.regAlloc->allocGPR(_It_, -1);
		const xRegister32& regS = mVU.regAlloc->allocGPR(_Is_, _Id_, mVUlow.backupVI);
		if (_It_ != _Is_)
			xOR(regS, regT);
		mVU.regAlloc->clearNeeded(regS);
		mVU.regAlloc->clearNeeded(regT);
		mVU.profiler.EmitOp(opIOR);
	}
	pass3 { mVUlog("IOR vi%02d, vi%02d, vi%02d", _Fd_, _Fs_, _Ft_); }
}

mVUop(mVU_ISUB)
{
	pass1 { mVUanalyzeIALU1(mVU, _Id_, _Is_, _It_); }
	pass2
	{
		if (_It_ != _Is_)
		{
			const xRegister32& regT = mVU.regAlloc->allocGPR(_It_, -1);
			const xRegister32& regS = mVU.regAlloc->allocGPR(_Is_, _Id_, mVUlow.backupVI);
			xSUB(regS, regT);
			mVU.regAlloc->clearNeeded(regS);
			mVU.regAlloc->clearNeeded(regT);
		}
		else
		{
			const xRegister32& regD = mVU.regAlloc->allocGPR(-1, _Id_, mVUlow.backupVI);
			xXOR(regD, regD);
			mVU.regAlloc->clearNeeded(regD);
		}
		mVU.profiler.EmitOp(opISUB);
	}
	pass3 { mVUlog("ISUB vi%02d, vi%02d, vi%02d", _Fd_, _Fs_, _Ft_); }
}

mVUop(mVU_ISUBIU)
{
	pass1 { mVUanalyzeIALU2(mVU, _Is_, _It_); }
	pass2
	{
		const xRegister32& regS = mVU.regAlloc->allocGPR(_Is_, _It_, mVUlow.backupVI);
		if (!EmuConfig.Gamefixes.IbitHack)
		{
			if (_Imm15_ != 0)
				xSUB(regS, _Imm15_);
		}
		else
		{
			xMOV(gprT1, ptr32[&curI]);
			xMOV(gprT2, gprT1);
			xSHR(gprT2, 10);
			xAND(gprT2, 0x7800);
			xAND(gprT1, 0x7FF);
			xOR(gprT1, gprT2);

			xSUB(regS, gprT1);
		}
		mVU.regAlloc->clearNeeded(regS);
		mVU.profiler.EmitOp(opISUBIU);
	}
	pass3 { mVUlog("ISUBIU vi%02d, vi%02d, %d", _Ft_, _Fs_, _Imm15_); }
}

//------------------------------------------------------------------
// MFIR/MFP/MOVE/MR32/MTIR
//------------------------------------------------------------------

mVUop(mVU_MFIR)
{
	pass1
	{
		if (!_Ft_)
		{
			mVUlow.isNOP = true;
		}
		analyzeVIreg1(mVU, _Is_, mVUlow.VI_read[0]);
		analyzeReg2  (mVU, _Ft_, mVUlow.VF_write, 1);
	}
	pass2
	{
		const xmm& Ft = mVU.regAlloc->allocReg(-1, _Ft_, _X_Y_Z_W);
		if (_Is_ != 0)
		{
			const xRegister32& regS = mVU.regAlloc->allocGPR(_Is_, -1);
			xMOVSX(xRegister32(regS), xRegister16(regS));
			// TODO: Broadcast instead
			xMOVDZX(Ft, regS);
			if (!_XYZW_SS)
				mVUunpack_xyzw(Ft, Ft, 0);
			mVU.regAlloc->clearNeeded(regS);
		}
		else
		{
			xPXOR(Ft, Ft);
		}
		mVU.regAlloc->clearNeeded(Ft);
		mVU.profiler.EmitOp(opMFIR);
	}
	pass3 { mVUlog("MFIR.%s vf%02d, vi%02d", _XYZW_String, _Ft_, _Fs_); }
}

mVUop(mVU_MFP)
{
	pass1
	{
		if (isVU0)
		{
			mVUlow.isNOP = true;
			return;
		}
		mVUanalyzeMFP(mVU, _Ft_);
	}
	pass2
	{
		const xmm& Ft = mVU.regAlloc->allocReg(-1, _Ft_, _X_Y_Z_W);
		getPreg(mVU, Ft);
		mVU.regAlloc->clearNeeded(Ft);
		mVU.profiler.EmitOp(opMFP);
	}
	pass3 { mVUlog("MFP.%s vf%02d, P", _XYZW_String, _Ft_); }
}

mVUop(mVU_MOVE)
{
	pass1 { mVUanalyzeMOVE(mVU, _Fs_, _Ft_); }
	pass2
	{
		const xmm& Fs = mVU.regAlloc->allocReg(_Fs_, _Ft_, _X_Y_Z_W);
		mVU.regAlloc->clearNeeded(Fs);
		mVU.profiler.EmitOp(opMOVE);
	}
	pass3 { mVUlog("MOVE.%s vf%02d, vf%02d", _XYZW_String, _Ft_, _Fs_); }
}

mVUop(mVU_MR32)
{
	pass1 { mVUanalyzeMR32(mVU, _Fs_, _Ft_); }
	pass2
	{
		const xmm& Fs = mVU.regAlloc->allocReg(_Fs_);
		const xmm& Ft = mVU.regAlloc->allocReg(-1, _Ft_, _X_Y_Z_W);
		if (_XYZW_SS)
			mVUunpack_xyzw(Ft, Fs, (_X ? 1 : (_Y ? 2 : (_Z ? 3 : 0))));
		else
			xPSHUF.D(Ft, Fs, 0x39);
		mVU.regAlloc->clearNeeded(Ft);
		mVU.regAlloc->clearNeeded(Fs);
		mVU.profiler.EmitOp(opMR32);
	}
	pass3 { mVUlog("MR32.%s vf%02d, vf%02d", _XYZW_String, _Ft_, _Fs_); }
}

mVUop(mVU_MTIR)
{
	pass1
	{
		if (!_It_)
			mVUlow.isNOP = true;

		analyzeReg5(mVU, _Fs_, _Fsf_, mVUlow.VF_read[0]);
		analyzeVIreg2(mVU, _It_, mVUlow.VI_write, 1);
	}
	pass2
	{
		const xmm& Fs = mVU.regAlloc->allocReg(_Fs_, 0, (1 << (3 - _Fsf_)));
		const xRegister32& regT = mVU.regAlloc->allocGPR(-1, _It_, mVUlow.backupVI);
		xMOVD(regT, Fs);
		mVU.regAlloc->clearNeeded(regT);
		mVU.regAlloc->clearNeeded(Fs);
		mVU.profiler.EmitOp(opMTIR);
	}
	pass3 { mVUlog("MTIR vi%02d, vf%02d%s", _Ft_, _Fs_, _Fsf_String); }
}

//------------------------------------------------------------------
// ILW/ILWR
//------------------------------------------------------------------

mVUop(mVU_ILW)
{
	pass1
	{
		if (!_It_)
			mVUlow.isNOP = true;

		analyzeVIreg1(mVU, _Is_, mVUlow.VI_read[0]);
		analyzeVIreg2(mVU, _It_, mVUlow.VI_write, 4);
	}
	pass2
	{
		void* ptr = mVU.regs().Mem + offsetSS;
		std::optional<xAddressVoid> optaddr(EmuConfig.Gamefixes.IbitHack ? std::nullopt : mVUoptimizeConstantAddr(mVU, _Is_, _Imm11_, offsetSS));
		if (!optaddr.has_value())
		{
			mVU.regAlloc->moveVIToGPR(gprT1, _Is_);
			if (!EmuConfig.Gamefixes.IbitHack)
			{
				if (_Imm11_ != 0)
					xADD(gprT1, _Imm11_);
			}
			else
			{
				xMOV(gprT2, ptr32[&curI]);
				xSHL(gprT2, 21);
				xSAR(gprT2, 21);

				xADD(gprT1, gprT2);
			}
			mVUaddrFix(mVU, gprT1q, gprT2q);
		}

		const xRegister32& regT = mVU.regAlloc->allocGPR(-1, _It_, mVUlow.backupVI);
		xMOVZX(regT, ptr16[optaddr.has_value() ? optaddr.value() : xComplexAddress(gprT2q, ptr, gprT1q)]);
		mVU.regAlloc->clearNeeded(regT);
		mVU.profiler.EmitOp(opILW);
	}
	pass3 { mVUlog("ILW.%s vi%02d, vi%02d + %d", _XYZW_String, _Ft_, _Fs_, _Imm11_); }
}

mVUop(mVU_ILWR)
{
	pass1
	{
		if (!_It_)
			mVUlow.isNOP = true;

		analyzeVIreg1(mVU, _Is_, mVUlow.VI_read[0]);
		analyzeVIreg2(mVU, _It_, mVUlow.VI_write, 4);
	}
	pass2
	{
		void* ptr = mVU.regs().Mem + offsetSS;
		if (_Is_)
		{
			mVU.regAlloc->moveVIToGPR(gprT1, _Is_);
			mVUaddrFix (mVU, gprT1q, gprT2q);

			const xRegister32& regT = mVU.regAlloc->allocGPR(-1, _It_, mVUlow.backupVI);
			xMOVZX(regT, ptr16[xComplexAddress(gprT2q, ptr, gprT1q)]);
			mVU.regAlloc->clearNeeded(regT);
		}
		else
		{
			const xRegister32& regT = mVU.regAlloc->allocGPR(-1, _It_, mVUlow.backupVI);
			xMOVZX(regT, ptr16[ptr]);
			mVU.regAlloc->clearNeeded(regT);
		}
		mVU.profiler.EmitOp(opILWR);
	}
	pass3 { mVUlog("ILWR.%s vi%02d, vi%02d", _XYZW_String, _Ft_, _Fs_); }
}

//------------------------------------------------------------------
// ISW/ISWR
//------------------------------------------------------------------

mVUop(mVU_ISW)
{
	pass1
	{
		mVUlow.isMemWrite = true;
		analyzeVIreg1(mVU, _Is_, mVUlow.VI_read[0]);
		analyzeVIreg1(mVU, _It_, mVUlow.VI_read[1]);
	}
	pass2
	{
		std::optional<xAddressVoid> optaddr(EmuConfig.Gamefixes.IbitHack ? std::nullopt : mVUoptimizeConstantAddr(mVU, _Is_, _Imm11_, 0));
		if (!optaddr.has_value())
		{
			mVU.regAlloc->moveVIToGPR(gprT1, _Is_);
			if (!EmuConfig.Gamefixes.IbitHack)
			{
				if (_Imm11_ != 0)
					xADD(gprT1, _Imm11_);
			}
			else
			{
				xMOV(gprT2, ptr32[&curI]);
				xSHL(gprT2, 21);
				xSAR(gprT2, 21);

				xADD(gprT1, gprT2);
			}
			mVUaddrFix(mVU, gprT1q, gprT2q);
		}

		// If regT is dirty, the high bits might not be zero.
		const xRegister32& regT = mVU.regAlloc->allocGPR(_It_, -1, false, true);
		const xAddressVoid ptr(optaddr.has_value() ? optaddr.value() : xComplexAddress(gprT2q, mVU.regs().Mem, gprT1q));
		if (_X) xMOV(ptr32[ptr], regT);
		if (_Y) xMOV(ptr32[ptr + 4], regT);
		if (_Z) xMOV(ptr32[ptr + 8], regT);
		if (_W) xMOV(ptr32[ptr + 12], regT);
		mVU.regAlloc->clearNeeded(regT);
		mVU.profiler.EmitOp(opISW);
	}
	pass3 { mVUlog("ISW.%s vi%02d, vi%02d + %d", _XYZW_String, _Ft_, _Fs_, _Imm11_); }
}

mVUop(mVU_ISWR)
{
	pass1
	{
		mVUlow.isMemWrite = true;
		analyzeVIreg1(mVU, _Is_, mVUlow.VI_read[0]);
		analyzeVIreg1(mVU, _It_, mVUlow.VI_read[1]);
	}
	pass2
	{
		void* base = mVU.regs().Mem;
		xAddressReg is = xEmptyReg;
		if (_Is_)
		{
			mVU.regAlloc->moveVIToGPR(gprT1, _Is_);
			mVUaddrFix(mVU, gprT1q, gprT2q);
			is = gprT1q;
		}
		const xRegister32& regT = mVU.regAlloc->allocGPR(_It_, -1, false, true);
		if (!is.IsEmpty() && (sptr)base != (s32)(sptr)base)
		{
			int register_offset = -1;
			auto writeBackAt = [&](int offset) {
				if (register_offset == -1)
				{
					xLEA(gprT2q, ptr[(void*)((sptr)base + offset)]);
					register_offset = offset;
				}
				xMOV(ptr32[gprT2q + is + (offset - register_offset)], regT);
			};
			if (_X) writeBackAt(0);
			if (_Y) writeBackAt(4);
			if (_Z) writeBackAt(8);
			if (_W) writeBackAt(12);
		}
		else if (is.IsEmpty())
		{
			if (_X) xMOV(ptr32[(void*)((uptr)base)], regT);
			if (_Y) xMOV(ptr32[(void*)((uptr)base + 4)], regT);
			if (_Z) xMOV(ptr32[(void*)((uptr)base + 8)], regT);
			if (_W) xMOV(ptr32[(void*)((uptr)base + 12)], regT);
		}
		else
		{
			if (_X) xMOV(ptr32[base + is], regT);
			if (_Y) xMOV(ptr32[base + is + 4], regT);
			if (_Z) xMOV(ptr32[base + is + 8], regT);
			if (_W) xMOV(ptr32[base + is + 12], regT);
		}
		mVU.regAlloc->clearNeeded(regT);

		mVU.profiler.EmitOp(opISWR);
	}
	pass3 { mVUlog("ISWR.%s vi%02d, vi%02d", _XYZW_String, _Ft_, _Fs_); }
}

//------------------------------------------------------------------
// LQ/LQD/LQI
//------------------------------------------------------------------

mVUop(mVU_LQ)
{
	pass1 { mVUanalyzeLQ(mVU, _Ft_, _Is_, false); }
	pass2
	{
		const std::optional<xAddressVoid> optaddr(EmuConfig.Gamefixes.IbitHack ? std::nullopt : mVUoptimizeConstantAddr(mVU, _Is_, _Imm11_, 0));
		if (!optaddr.has_value())
		{
			mVU.regAlloc->moveVIToGPR(gprT1, _Is_);
			if (!EmuConfig.Gamefixes.IbitHack)
			{
				if (_Imm11_ != 0)
					xADD(gprT1, _Imm11_);
			}
			else
			{
				xMOV(gprT2, ptr32[&curI]);
				xSHL(gprT2, 21);
				xSAR(gprT2, 21);

				xADD(gprT1, gprT2);
			}
			mVUaddrFix(mVU, gprT1q, gprT2q);
		}

		const xmm& Ft = mVU.regAlloc->allocReg(-1, _Ft_, _X_Y_Z_W);
		mVUloadReg(Ft, optaddr.has_value() ? optaddr.value() : xComplexAddress(gprT2q, mVU.regs().Mem, gprT1q), _X_Y_Z_W);
		mVU.regAlloc->clearNeeded(Ft);
		mVU.profiler.EmitOp(opLQ);
	}
	pass3 { mVUlog("LQ.%s vf%02d, vi%02d + %d", _XYZW_String, _Ft_, _Fs_, _Imm11_); }
}

mVUop(mVU_LQD)
{
	pass1 { mVUanalyzeLQ(mVU, _Ft_, _Is_, true); }
	pass2
	{
		void* ptr = mVU.regs().Mem;
		xAddressReg is = xEmptyReg;
		if (_Is_ || isVU0) // Access VU1 regs mem-map in !_Is_ case
		{
			const xRegister32& regS = mVU.regAlloc->allocGPR(_Is_, _Is_, mVUlow.backupVI);
			xDEC(regS);
			xMOVSX(gprT1, xRegister16(regS)); // TODO: Confirm
			mVU.regAlloc->clearNeeded(regS);
			mVUaddrFix(mVU, gprT1q, gprT2q);
			is = gprT1q;
		}
		else
		{
			ptr = (void*)((sptr)ptr + (0xffff & (mVU.microMemSize - 8)));
		}
		if (!mVUlow.noWriteVF)
		{
			const xmm& Ft = mVU.regAlloc->allocReg(-1, _Ft_, _X_Y_Z_W);
			if (is.IsEmpty())
			{
				mVUloadReg(Ft, xAddressVoid(ptr), _X_Y_Z_W);
			}
			else
			{
				mVUloadReg(Ft, xComplexAddress(gprT2q, ptr, is), _X_Y_Z_W);
			}
			mVU.regAlloc->clearNeeded(Ft);
		}
		mVU.profiler.EmitOp(opLQD);
	}
	pass3 { mVUlog("LQD.%s vf%02d, --vi%02d", _XYZW_String, _Ft_, _Is_); }
}

mVUop(mVU_LQI)
{
	pass1 { mVUanalyzeLQ(mVU, _Ft_, _Is_, true); }
	pass2
	{
		void* ptr = mVU.regs().Mem;
		xAddressReg is = xEmptyReg;
		if (_Is_)
		{
			const xRegister32& regS = mVU.regAlloc->allocGPR(_Is_, _Is_, mVUlow.backupVI);
			xMOVSX(gprT1, xRegister16(regS)); // TODO: Confirm
			xINC(regS);
			mVU.regAlloc->clearNeeded(regS);
			mVUaddrFix(mVU, gprT1q, gprT2q);
			is = gprT1q;
		}
		if (!mVUlow.noWriteVF)
		{
			const xmm& Ft = mVU.regAlloc->allocReg(-1, _Ft_, _X_Y_Z_W);
			if (is.IsEmpty())
				mVUloadReg(Ft, xAddressVoid(ptr), _X_Y_Z_W);
			else
				mVUloadReg(Ft, xComplexAddress(gprT2q, ptr, is), _X_Y_Z_W);
			mVU.regAlloc->clearNeeded(Ft);
		}
		mVU.profiler.EmitOp(opLQI);
	}
	pass3 { mVUlog("LQI.%s vf%02d, vi%02d++", _XYZW_String, _Ft_, _Fs_); }
}

//------------------------------------------------------------------
// SQ/SQD/SQI
//------------------------------------------------------------------

static __fi void mVU_XGKICK_SYNC_SQI(mV);

mVUop(mVU_SQ)
{
	pass1 { mVUanalyzeSQ(mVU, _Fs_, _It_, false); }
	pass2
	{
		const std::optional<xAddressVoid> optptr(EmuConfig.Gamefixes.IbitHack ? std::nullopt : mVUoptimizeConstantAddr(mVU, _It_, _Imm11_, 0));
		if (!optptr.has_value())
		{
			mVU.regAlloc->moveVIToGPR(gprT1, _It_);
			if (!EmuConfig.Gamefixes.IbitHack)
			{
				if (_Imm11_ != 0)
					xADD(gprT1, _Imm11_);
			}
			else
			{
				xMOV(gprT2, ptr32[&curI]);
				xSHL(gprT2, 21);
				xSAR(gprT2, 21);

				xADD(gprT1, gprT2);
			}
			mVUaddrFix(mVU, gprT1q, gprT2q);
		}
		const xmm& Fs = mVU.regAlloc->allocReg(_Fs_, _XYZW_PS ? -1 : 0, _X_Y_Z_W);
		mVUsaveReg(Fs, optptr.has_value() ? optptr.value() : xComplexAddress(gprT2q, mVU.regs().Mem, gprT1q), _X_Y_Z_W, 1);
		mVU.regAlloc->clearNeeded(Fs);
		mVU.profiler.EmitOp(opSQ);
	}
	pass3 { mVUlog("SQ.%s vf%02d, vi%02d + %d", _XYZW_String, _Fs_, _Ft_, _Imm11_); }
}

mVUop(mVU_SQD)
{
	pass1 { mVUanalyzeSQ(mVU, _Fs_, _It_, true); }
	pass2
	{
		void* ptr = mVU.regs().Mem;
		xAddressReg it = xEmptyReg;
		if (_It_ || isVU0) // Access VU1 regs mem-map in !_It_ case
		{
			const xRegister32& regT = mVU.regAlloc->allocGPR(_It_, _It_, mVUlow.backupVI);
			xDEC(regT);
			xMOVZX(gprT1, xRegister16(regT));
			mVU.regAlloc->clearNeeded(regT);
			mVUaddrFix(mVU, gprT1q, gprT2q);
			it = gprT1q;
		}
		else
		{
			ptr = (void*)((sptr)ptr + (0xffff & (mVU.microMemSize - 8)));
		}
		const xmm& Fs = mVU.regAlloc->allocReg(_Fs_, _XYZW_PS ? -1 : 0, _X_Y_Z_W);
		if (it.IsEmpty())
			mVUsaveReg(Fs, xAddressVoid(ptr), _X_Y_Z_W, 1);
		else
			mVUsaveReg(Fs, xComplexAddress(gprT2q, ptr, it), _X_Y_Z_W, 1);
		mVU.regAlloc->clearNeeded(Fs);
		mVU.profiler.EmitOp(opSQD);
	}
	pass3 { mVUlog("SQD.%s vf%02d, --vi%02d", _XYZW_String, _Fs_, _Ft_); }
}

mVUop(mVU_SQI)
{
	pass1
	{
		mVUanalyzeSQ(mVU, _Fs_, _It_, true);
		mVUlow.deferXgkickSync = isVU1 && !THREAD_VU1 && _It_ != 0;
	}
	pass2
	{
		void* ptr = mVU.regs().Mem;
		if (_It_)
		{
			const xRegister32& regT = mVU.regAlloc->allocGPR(_It_, _It_, mVUlow.backupVI);
			xMOVZX(gprT1, xRegister16(regT));
			xINC(regT);
			mVU.regAlloc->clearNeeded(regT);
			mVUaddrFix(mVU, gprT1q, gprT2q);
		}
		if (mVUlow.deferXgkickSync && CHECK_XGKICKHACK)
			mVU_XGKICK_SYNC_SQI(mVU);
		const xmm& Fs = mVU.regAlloc->allocReg(_Fs_, _XYZW_PS ? -1 : 0, _X_Y_Z_W);
		if (_It_)
			mVUsaveReg(Fs, xComplexAddress(gprT2q, ptr, gprT1q), _X_Y_Z_W, 1);
		else
			mVUsaveReg(Fs, xAddressVoid(ptr), _X_Y_Z_W, 1);
		mVU.regAlloc->clearNeeded(Fs);
		mVU.profiler.EmitOp(opSQI);
	}
	pass3 { mVUlog("SQI.%s vf%02d, vi%02d++", _XYZW_String, _Fs_, _Ft_); }
}

//------------------------------------------------------------------
// RINIT/RGET/RNEXT/RXOR
//------------------------------------------------------------------

mVUop(mVU_RINIT)
{
	pass1 { mVUanalyzeR1(mVU, _Fs_, _Fsf_); }
	pass2
	{
		if (_Fs_ || (_Fsf_ == 3))
		{
			const xmm& Fs = mVU.regAlloc->allocReg(_Fs_, 0, (1 << (3 - _Fsf_)));
			xMOVD(gprT1, Fs);
			xAND(gprT1, 0x007fffff);
			xOR (gprT1, 0x3f800000);
			xMOV(ptr32[Rmem], gprT1);
			mVU.regAlloc->clearNeeded(Fs);
		}
		else
			xMOV(ptr32[Rmem], 0x3f800000);
		mVU.profiler.EmitOp(opRINIT);
	}
	pass3 { mVUlog("RINIT R, vf%02d%s", _Fs_, _Fsf_String); }
}

static __fi void mVU_RGET_(mV, const x32& Rreg)
{
	if (!mVUlow.noWriteVF)
	{
		const xmm& Ft = mVU.regAlloc->allocReg(-1, _Ft_, _X_Y_Z_W);
		xMOVDZX(Ft, Rreg);
		if (!_XYZW_SS)
			mVUunpack_xyzw(Ft, Ft, 0);
		mVU.regAlloc->clearNeeded(Ft);
	}
}

mVUop(mVU_RGET)
{
	pass1 { mVUanalyzeR2(mVU, _Ft_, true); }
	pass2
	{
		xMOV(gprT1, ptr32[Rmem]);
		mVU_RGET_(mVU, gprT1);
		mVU.profiler.EmitOp(opRGET);
	}
	pass3 { mVUlog("RGET.%s vf%02d, R", _XYZW_String, _Ft_); }
}

mVUop(mVU_RNEXT)
{
	pass1 { mVUanalyzeR2(mVU, _Ft_, false); }
	pass2
	{
		// algorithm from www.project-fao.org
		const xRegister32& temp3 = mVU.regAlloc->allocGPR();
		xMOV(temp3, ptr32[Rmem]);
		xMOV(gprT1, temp3);
		xSHR(gprT1, 4);
		xAND(gprT1, 1);

		xMOV(gprT2, temp3);
		xSHR(gprT2, 22);
		xAND(gprT2, 1);

		xSHL(temp3, 1);
		xXOR(gprT1, gprT2);
		xXOR(temp3, gprT1);
		xAND(temp3, 0x007fffff);
		xOR (temp3, 0x3f800000);
		xMOV(ptr32[Rmem], temp3);
		mVU_RGET_(mVU, temp3);
		mVU.regAlloc->clearNeeded(temp3);
		mVU.profiler.EmitOp(opRNEXT);
	}
	pass3 { mVUlog("RNEXT.%s vf%02d, R", _XYZW_String, _Ft_); }
}

mVUop(mVU_RXOR)
{
	pass1 { mVUanalyzeR1(mVU, _Fs_, _Fsf_); }
	pass2
	{
		if (_Fs_ || (_Fsf_ == 3))
		{
			const xmm& Fs = mVU.regAlloc->allocReg(_Fs_, 0, (1 << (3 - _Fsf_)));
			xMOVD(gprT1, Fs);
			xAND(gprT1, 0x7fffff);
			xXOR(ptr32[Rmem], gprT1);
			mVU.regAlloc->clearNeeded(Fs);
		}
		mVU.profiler.EmitOp(opRXOR);
	}
	pass3 { mVUlog("RXOR R, vf%02d%s", _Fs_, _Fsf_String); }
}

//------------------------------------------------------------------
// WaitP/WaitQ
//------------------------------------------------------------------

mVUop(mVU_WAITP)
{
	pass1
	{
		if (isVU0)
		{
			mVUlow.isNOP = true;
			return;
		}
		mVUstall = std::max(mVUstall, (u8)((mVUregs.p) ? (mVUregs.p - 1) : 0));
	}
	pass2 { mVU.profiler.EmitOp(opWAITP); }
	pass3 { mVUlog("WAITP"); }
}

mVUop(mVU_WAITQ)
{
	pass1
	{
		mVUstall = std::max(mVUstall, mVUregs.q);
		if (!CHECK_VU_SOFT(mVU.index))
			mVUinfo.doDivFlag = 1;
	}
	pass2
	{
		if (CHECK_VU_SOFT(mVU.index) && mVUinfo.doDivFlag && mVUstall >= 4)
		{
			// The stall drains earlier FMAC/FSSET writes. Publish Q's causes now,
			// including in the delayed instance of an FMAC paired with WAITQ.
			for (int i = 0; i < 4; i++)
			{
				xAND(getFlagReg(i), ~0xc0000u);
				xOR(getFlagReg(i), ptr32[&mVU.divFlag]);
			}
		}
		else if (!CHECK_VU_SOFT(mVU.index))
		{
			if (!sFLAG.doFlag)
				xMOV(getFlagReg(sFLAG.write), getFlagReg(sFLAG.lastWrite));
			xAND(getFlagReg(sFLAG.write), 0xfff3ffff);
			xOR(getFlagReg(sFLAG.write), ptr32[&mVU.divFlag]);
		}
		mVU.profiler.EmitOp(opWAITQ);
	}
	pass3 { mVUlog("WAITQ"); }
}

//------------------------------------------------------------------
// XTOP/XITOP
//------------------------------------------------------------------

mVUop(mVU_XTOP)
{
	pass1
	{
		if (isVU0)
		{
			mVUlow.isNOP = true;
			return;
		}

		if (!_It_)
			mVUlow.isNOP = true;

		analyzeVIreg2(mVU, _It_, mVUlow.VI_write, 1);
	}
	pass2
	{
		const xRegister32& regT = mVU.regAlloc->allocGPR(-1, _It_, mVUlow.backupVI);
		xMOVZX(regT, ptr16[&mVU.getVifRegs().top]);
		mVU.regAlloc->clearNeeded(regT);
		mVU.profiler.EmitOp(opXTOP);
	}
	pass3 { mVUlog("XTOP vi%02d", _Ft_); }
}

mVUop(mVU_XITOP)
{
	pass1
	{
		if (!_It_)
			mVUlow.isNOP = true;

		analyzeVIreg2(mVU, _It_, mVUlow.VI_write, 1);
	}
	pass2
	{
		const xRegister32& regT = mVU.regAlloc->allocGPR(-1, _It_, mVUlow.backupVI);
		xMOVZX(regT, ptr16[&mVU.getVifRegs().itop]);
		xAND(regT, isVU1 ? 0x3ff : 0xff);
		mVU.regAlloc->clearNeeded(regT);
		mVU.profiler.EmitOp(opXITOP);
	}
	pass3 { mVUlog("XITOP vi%02d", _Ft_); }
}

//------------------------------------------------------------------
// XGkick
//------------------------------------------------------------------

void mVU_XGKICK_(u32 addr)
{
	addr = (addr & 0x3ff) * 16;
	u32 diff = 0x4000 - addr;
	u32 size = gifUnit.GetGSPacketSize(GIF_PATH_1, vuRegs[1].Mem, addr, ~0u, true);

	if (size > diff)
	{
		//DevCon.WriteLn(Color_Green, "microVU1: XGkick Wrap!");
		gifUnit.gifPath[GIF_PATH_1].CopyGSPacketData(&vuRegs[1].Mem[addr], diff, true);
		gifUnit.TransferGSPacketData(GIF_TRANS_XGKICK, &vuRegs[1].Mem[0], size - diff, true);
	}
	else
	{
		gifUnit.TransferGSPacketData(GIF_TRANS_XGKICK, &vuRegs[1].Mem[addr], size, true);
	}
}

void _vuXGKICKTransfermVU(bool flush)
{
	static constexpr u32 buffered_packet = 0x80000000u;
	while (VU1.xgkickenable && (flush || VU1.xgkickcyclecount >= 2))
	{
		u32 transfersize = 0;
		if (VU1.xgkickdiff & buffered_packet)
		{
			VU1.xgkickdiff &= ~buffered_packet;
			gifUnit.lastTranType = GIF_TRANS_XGKICK;
			if (!gifUnit.CanDoPath1())
				gifUnit.stat.P1Q = 1;
			gifUnit.Execute(false, false);
			if (VU1.xgkickendpacket)
			{
				VU1.xgkickenable = false;
				break;
			}
		}

		if (VU1.xgkicksizeremaining == 0)
		{
			u32 size = gifUnit.GetGSPacketSize(GIF_PATH_1, vuRegs[1].Mem, VU1.xgkickaddr, ~0u, flush);
			VU1.xgkicksizeremaining = size & 0xFFFF;
			VU1.xgkickendpacket = size >> 31;
			VU1.xgkickdiff = 0x4000 - VU1.xgkickaddr;

			if (VU1.xgkicksizeremaining == 0)
			{
				VU1.xgkickenable = false;
				break;
			}
		}

		if (!flush)
		{
			transfersize = std::min(VU1.xgkicksizeremaining, VU1.xgkickcyclecount * 8);
			transfersize = std::min(transfersize, VU1.xgkickdiff);
		}
		else
		{
			transfersize = VU1.xgkicksizeremaining;
			transfersize = std::min(transfersize, VU1.xgkickdiff);
		}

		// Would be "nicer" to do the copy until it's all up, however this really screws up PATH3 masking stuff
		// So lets just do it the other way :)
		if (THREAD_VU1)
		{
			if (transfersize < VU1.xgkicksizeremaining)
				gifUnit.gifPath[GIF_PATH_1].CopyGSPacketData(&VU1.Mem[VU1.xgkickaddr], transfersize, true);
			else
				gifUnit.TransferGSPacketData(GIF_TRANS_XGKICK, &vuRegs[1].Mem[VU1.xgkickaddr], transfersize, true);
		}
		else
		{
			gifUnit.TransferGSPacketData(GIF_TRANS_XGKICK, &vuRegs[1].Mem[VU1.xgkickaddr], transfersize, true);
		}

		if (flush)
			VU1.cycle += transfersize / 8;

		VU1.xgkickcyclecount -= transfersize / 8;

		VU1.xgkickaddr = (VU1.xgkickaddr + transfersize) & 0x3FFF;
		VU1.xgkicksizeremaining -= transfersize;
		VU1.xgkickdiff = 0x4000 - VU1.xgkickaddr;

		if (VU1.xgkickendpacket && !VU1.xgkicksizeremaining)
		{
			VU1.xgkickenable = false;
			// Check if VIF is waiting for the GIF to not be busy
		}
	}
}

void _vuXGKICKPreparemVU()
{
	const u32 size = gifUnit.GetGSPacketSize(GIF_PATH_1, vuRegs[1].Mem,
		VU1.xgkickaddr, ~0u, true);
	VU1.xgkicksizeremaining = size & 0xffff;
	VU1.xgkickendpacket = size >> 31;
	VU1.xgkickdiff = 0x4000 - VU1.xgkickaddr;
	if (VU1.xgkicksizeremaining == 0)
		VU1.xgkickenable = false;
}

static __fi void mVUclampXgkickBytes(const xRegister32& byte_count)
{
	xCMP(byte_count, ptr32[&VU1.xgkicksizeremaining]);
	xCMOVA(byte_count, ptr32[&VU1.xgkicksizeremaining]);
	xCMP(byte_count, ptr32[&VU1.xgkickdiff]);
	xCMOVA(byte_count, ptr32[&VU1.xgkickdiff]);
}

static __fi void mVU_XGKICK_SYNC(mV, bool flush)
{
	if (flush)
	{
		mVU.regAlloc->flushCallerSavedRegisters();
		xTEST(ptr32[&VU1.xgkickenable], 0x1);
		xForwardJZ32 flush_skip_xgkick;
		xADD(ptr32[&VU1.xgkickcyclecount], mVUlow.kickcycles);
		mVUbackupRegs(mVU, true, true);
		xFastCall(_vuXGKICKTransfermVU, true);
		mVUrestoreRegs(mVU, true, true);
		flush_skip_xgkick.SetTarget();
		return;
	}

	// Add the single cycle remainder after this instruction, some games do the store
	// on the second instruction after the kick and that needs to go through first
	// but that's VERY close..
	xTEST(ptr32[&VU1.xgkickenable], 0x1);
	xForwardJZ32 skipxgkick;
	xADD(ptr32[&VU1.xgkickcyclecount], mVUlow.kickcycles - 1);
	xCMP(ptr32[&VU1.xgkickcyclecount], 2);
	xForwardJL32 needcycles;

	if (THREAD_VU1)
	{
		mVUbackupRegs(mVU, true, true);
		xFastCall(_vuXGKICKTransfermVU, false);
		mVUrestoreRegs(mVU, true, true);
	}
	else
	{
		Gif_Path& path = gifUnit.gifPath[GIF_PATH_1];
		const xRegister32& transfer_size = r10d;
		const xRegister32& transferred_size = r9d;
		const xRegister32& copy_value = r11d;
		std::array<std::optional<xForwardJump32>, 4> fast_failures;
		xPUSH(r9);
		xPUSH(r10);
		xPUSH(r11);
		xCMP(ptr32[&VU1.xgkicksizeremaining], 0);
		fast_failures[0].emplace(Jcc_Equal);
		xTEST(ptr32[&VU1.xgkickdiff], 0x80000000u);
		fast_failures[1].emplace(Jcc_NotZero);
		xMOV(transfer_size, ptr32[&VU1.xgkickcyclecount]);
		xSHL(transfer_size, 3);
		mVUclampXgkickBytes(transfer_size);
		xMOV(transferred_size, transfer_size);
		xMOV(gprT1, ptr32[&path.curSize]);
		xADD(gprT1, transfer_size);
		xCMP(gprT1, ptr32[&path.buffSize]);
		fast_failures[2].emplace(Jcc_Above);

		// Mirror CopyGSPacketData()'s MTGS ownership check before writing directly.
		// MTGS only decreases readAmount, so its concurrent progress can only make a
		// failed check conservative. Plain aligned loads have acquire semantics on x86.
		xMOV(gprT2, ptr32[&path.readAmount]);
		xADD(gprT2, ptr32[&path.gsPack.readAmount]);
		xForwardJZ32 fast_no_pending_reads;
		xNEG(gprT2);
		xADD(gprT2, ptr32[&path.curOffset]);
		xSUB(gprT2, ptr32[&path.gsPack.size]);
		xForwardJGE32 fast_reads_behind_write;
		xADD(gprT2, ptr32[&path.buffLimit]);
		xCMP(gprT2, gprT1);
		fast_failures[3].emplace(Jcc_LessOrEqual);
		fast_no_pending_reads.SetTarget();
		fast_reads_behind_write.SetTarget();

		// Buffer all bytes at their hardware read time. Parser-visible work is deferred
		// until a full XGKICK boundary, where allocator state is already synchronized.
		xMOV(gprT1q, ptr64[&path.buffer]);
		xMOV(gprT2, ptr32[&path.curSize]);
		xADD(gprT1q, gprT2q);
		xMOV64(gprT2q, reinterpret_cast<uptr>(VU1.Mem));
		xMOV(copy_value, ptr32[&VU1.xgkickaddr]);
		xADD(gprT2q, r11);
		xADD(ptr32[&path.curSize], transfer_size);
		u8* const copy_loop = xGetPtr();
		xMOV(r11, ptr64[gprT2q]);
		xMOV(ptr64[gprT1q], r11);
		xADD(gprT1q, 8);
		xADD(gprT2q, 8);
		xSUB(transfer_size, 8);
		xJcc32(Jcc_NotZero,
			static_cast<s32>(reinterpret_cast<sptr>(copy_loop) - (reinterpret_cast<sptr>(xGetPtr()) + 6)));

		xMOV(copy_value, transferred_size);
		xSHR(copy_value, 3);
		xSUB(ptr32[&VU1.xgkickcyclecount], copy_value);
		xADD(ptr32[&VU1.xgkickaddr], transferred_size);
		xSUB(ptr32[&VU1.xgkicksizeremaining], transferred_size);
		xSUB(ptr32[&VU1.xgkickdiff], transferred_size);
		xCMP(ptr32[&VU1.xgkickaddr], 0x4000);
		xForwardJNE8 fast_no_wrap;
		xMOV(ptr32[&VU1.xgkickaddr], 0);
		xMOV(ptr32[&VU1.xgkickdiff], 0x4000);
		fast_no_wrap.SetTarget();
		xCMP(ptr32[&VU1.xgkicksizeremaining], 0);
		xForwardJNE8 fast_packet_incomplete;
		xOR(ptr32[&VU1.xgkickdiff], 0x80000000u);
		fast_packet_incomplete.SetTarget();
		xForwardJump32 fast_success;

		for (std::optional<xForwardJump32>& failure : fast_failures)
			failure->SetTarget();
		xPOP(r11);
		xPOP(r10);
		xPOP(r9);
		mVUbackupRegs(mVU, true, true);
		xFastCall(_vuXGKICKTransfermVU, false);
		mVUrestoreRegs(mVU, true, true);
		xForwardJump32 fast_finished;
		fast_success.SetTarget();
		xPOP(r11);
		xPOP(r10);
		xPOP(r9);
		fast_finished.SetTarget();
	}
	needcycles.SetTarget();
	xADD(ptr32[&VU1.xgkickcyclecount], 1);
	skipxgkick.SetTarget();
}

static __fi void mVU_XGKICK_SYNC_SQI(mV)
{
	xTEST(ptr32[&VU1.xgkickenable], 0x1);
	xForwardJZ32 skip_store_sync;
	xPUSH(gprT1q);
	xPUSH(r9);
	xPUSH(r10);
	xPUSH(r11);

	std::array<std::optional<xForwardJump32>, 3> no_overlap;
	xMOV(r9d, ptr32[&VU1.xgkickcyclecount]);
	xADD(r9d, mVUlow.kickcycles);
	xCMP(r9d, 2);
	no_overlap[0].emplace(Jcc_Less);
	xSHL(r9d, 3);
	mVUclampXgkickBytes(r9d);
	xMOV(r10d, ptr32[&VU1.xgkickaddr]);
	xADD(r9d, r10d);
	xCMP(gprT1, r9d);
	no_overlap[1].emplace(Jcc_AboveOrEqual);
	xMOV(r11d, gprT1);
	xADD(r11d, 16);
	xCMP(r11d, r10d);
	no_overlap[2].emplace(Jcc_BelowOrEqual);

	mVU_XGKICK_SYNC(mVU, false);
	xForwardJump32 store_sync_finished;
	for (std::optional<xForwardJump32>& branch : no_overlap)
		branch->SetTarget();
	xADD(ptr32[&VU1.xgkickcyclecount], mVUlow.kickcycles);
	store_sync_finished.SetTarget();
	xPOP(r11);
	xPOP(r10);
	xPOP(r9);
	xPOP(gprT1q);
	skip_store_sync.SetTarget();
}

static __fi void mVU_XGKICK_DELAY(mV)
{
	mVU.regAlloc->flushCallerSavedRegisters();

	mVUbackupRegs(mVU, true, true);
	xFastCall(mVU_XGKICK_, ptr32[&mVU.VIxgkick]);
	mVUrestoreRegs(mVU, true, true);
}

mVUop(mVU_XGKICK)
{
	pass1
	{
		if (isVU0)
		{
			mVUlow.isNOP = true;
			return;
		}
		mVUanalyzeXGkick(mVU, _Is_, 1);
	}
	pass2
	{
		if (CHECK_XGKICKHACK)
		{
			mVUlow.kickcycles = 99;
			mVU_XGKICK_SYNC(mVU, true);
			mVUlow.kickcycles = 0;
		}
		if (mVUinfo.doXGKICK) // check for XGkick Transfer
		{
			mVU_XGKICK_DELAY(mVU);
			mVUinfo.doXGKICK = false;
		}

		const xRegister32& regS = mVU.regAlloc->allocGPR(_Is_, -1);
		if (!CHECK_XGKICKHACK)
		{
			xMOV(ptr32[&mVU.VIxgkick], regS);
		}
		else
		{
			xMOV(ptr32[&VU1.xgkickenable], 1);
			xMOV(ptr32[&VU1.xgkickendpacket], 0);
			xMOV(ptr32[&VU1.xgkicksizeremaining], 0);
			xMOV(ptr32[&VU1.xgkickcyclecount], 0);
			xMOV(gprT2, ptr32[&mVU.totalCycles]);
			xSUB(gprT2, ptr32[&mVU.cycles]);
			xADD(gprT2, ptr64[&VU1.cycle]);
			xMOV(ptr32[&VU1.xgkicklastcycle], gprT2);
			xMOV(gprT1, regS);
			xAND(gprT1, 0x3FF);
			xSHL(gprT1, 4);
			xMOV(ptr32[&VU1.xgkickaddr], gprT1);
		}
		mVU.regAlloc->clearNeeded(regS);
		if (CHECK_XGKICKHACK && !THREAD_VU1)
		{
			mVU.regAlloc->flushCallerSavedRegisters();
			mVUbackupRegs(mVU, true, true);
			xFastCall((void*)_vuXGKICKPreparemVU);
			mVUrestoreRegs(mVU, true, true);
		}
		mVU.profiler.EmitOp(opXGKICK);
	}
	pass3 { mVUlog("XGKICK vi%02d", _Fs_); }
}

//------------------------------------------------------------------
// Branches/Jumps
//------------------------------------------------------------------

void setBranchA(mP, int x, int _x_)
{
	bool isBranchDelaySlot = false;

	incPC(-2);
	if (mVUlow.branch)
		isBranchDelaySlot = true;
	incPC(2);

	pass1
	{
		if (_Imm11_ == 1 && !_x_ && !isBranchDelaySlot)
		{
			DevCon.WriteLn(Color_Green, "microVU%d: Branch Optimization", mVU.index);
			mVUlow.isNOP = true;
			return;
		}
		mVUbranch     = x;
		mVUlow.branch = x;
	}
	pass2 { if (_Imm11_ == 1 && !_x_ && !isBranchDelaySlot) { return; } mVUbranch = x; }
	pass3 { mVUbranch = x; }
	pass4 { if (_Imm11_ == 1 && !_x_ && !isBranchDelaySlot) { return; } mVUbranch = x; }
}

void condEvilBranch(mV, int JMPcc)
{
	if (mVUlow.badBranch)
	{
		xMOV(ptr32[&mVU.branch], gprT1);
		xMOV(ptr32[&mVU.badBranch], branchAddr(mVU));

		xCMP(gprT1b, 0);
		xForwardJump8 cJMP((JccComparisonType)JMPcc);
			incPC(4); // Branch Not Taken Addr
			xMOV(ptr32[&mVU.badBranch], xPC);
			incPC(-4);
		cJMP.SetTarget();
		return;
	}
	if (isEvilBlock)
	{
		xMOV(ptr32[&mVU.evilevilBranch], branchAddr(mVU));
		xCMP(gprT1b, 0);
		xForwardJump8 cJMP((JccComparisonType)JMPcc);
		xMOV(gprT1, ptr32[&mVU.evilBranch]); // Branch Not Taken
		xADD(gprT1, 8); // We have already executed 1 instruction from the original branch
		xMOV(ptr32[&mVU.evilevilBranch], gprT1);
		cJMP.SetTarget();
	}
	else
	{
		xMOV(ptr32[&mVU.evilBranch], branchAddr(mVU));
		xCMP(gprT1b, 0);
		xForwardJump8 cJMP((JccComparisonType)JMPcc);
		xMOV(gprT1, ptr32[&mVU.badBranch]); // Branch Not Taken
		xADD(gprT1, 8); // We have already executed 1 instruction from the original branch
		xMOV(ptr32[&mVU.evilBranch], gprT1);
		cJMP.SetTarget();
		incPC(-2);
		if (mVUlow.branch >= 9)
			DevCon.Warning("Conditional in JALR/JR delay slot - If game broken report to PCSX2 Team");
		incPC(2);
	}
}

mVUop(mVU_B)
{
	setBranchA(mX, 1, 0);
	pass1 { mVUanalyzeNormBranch(mVU, 0, false); }
	pass2
	{
		if (mVUlow.badBranch)  { xMOV(ptr32[&mVU.badBranch],  branchAddr(mVU)); }
		if (mVUlow.evilBranch) { if (isEvilBlock) xMOV(ptr32[&mVU.evilevilBranch], branchAddr(mVU)); else xMOV(ptr32[&mVU.evilBranch], branchAddr(mVU)); }
		mVU.profiler.EmitOp(opB);
	}
	pass3 { mVUlog("B [<a href=\"#addr%04x\">%04x</a>]", branchAddr(mVU), branchAddr(mVU)); }
}

mVUop(mVU_BAL)
{
	setBranchA(mX, 2, _It_);
	pass1 { mVUanalyzeNormBranch(mVU, _It_, true); }
	pass2
	{
		if (!mVUlow.evilBranch)
		{
			const xRegister32& regT = mVU.regAlloc->allocGPR(-1, _It_, mVUlow.backupVI);
			xMOV(regT, bSaveAddr);
			mVU.regAlloc->clearNeeded(regT);
		}
		else
		{
			incPC(-2);
			DevCon.Warning("Linking BAL from %s branch taken/not taken target! - If game broken report to PCSX2 Team", branchSTR[mVUlow.branch & 0xf]);
			incPC(2);

			const xRegister32& regT = mVU.regAlloc->allocGPR(-1, _It_, mVUlow.backupVI);
			if (isEvilBlock)
				xMOV(regT, ptr32[&mVU.evilBranch]);
			else
				xMOV(regT, ptr32[&mVU.badBranch]);

			xADD(regT, 8);
			xSHR(regT, 3);
			mVU.regAlloc->clearNeeded(regT);
		}

		if (mVUlow.badBranch)  { xMOV(ptr32[&mVU.badBranch],  branchAddr(mVU)); }
		if (mVUlow.evilBranch) { if (isEvilBlock) xMOV(ptr32[&mVU.evilevilBranch], branchAddr(mVU)); else xMOV(ptr32[&mVU.evilBranch], branchAddr(mVU)); }
		mVU.profiler.EmitOp(opBAL);
	}
	pass3 { mVUlog("BAL vi%02d [<a href=\"#addr%04x\">%04x</a>]", _Ft_, branchAddr(mVU), branchAddr(mVU)); }
}

mVUop(mVU_IBEQ)
{
	setBranchA(mX, 3, 0);
	pass1 { mVUanalyzeCondBranch2(mVU, _Is_, _It_); }
	pass2
	{
		if (mVUlow.memReadIs)
			xMOV(gprT1, ptr32[&mVU.VIbackup]);
		else
			mVU.regAlloc->moveVIToGPR(gprT1, _Is_);

		if (mVUlow.memReadIt)
			xXOR(gprT1, ptr32[&mVU.VIbackup]);
		else
		{
			const xRegister32& regT = mVU.regAlloc->allocGPR(_It_);
			xXOR(gprT1, regT);
			mVU.regAlloc->clearNeeded(regT);
		}

		if (!(isBadOrEvil))
			xMOV(ptr32[&mVU.branch], gprT1);
		else
			condEvilBranch(mVU, Jcc_Equal);
		mVU.profiler.EmitOp(opIBEQ);
	}
	pass3 { mVUlog("IBEQ vi%02d, vi%02d [<a href=\"#addr%04x\">%04x</a>]", _Ft_, _Fs_, branchAddr(mVU), branchAddr(mVU)); }
}

mVUop(mVU_IBGEZ)
{
	setBranchA(mX, 4, 0);
	pass1 { mVUanalyzeCondBranch1(mVU, _Is_); }
	pass2
	{
		if (mVUlow.memReadIs)
			xMOV(gprT1, ptr32[&mVU.VIbackup]);
		else
			mVU.regAlloc->moveVIToGPR(gprT1, _Is_);
		if (!(isBadOrEvil))
			xMOV(ptr32[&mVU.branch], gprT1);
		else
			condEvilBranch(mVU, Jcc_GreaterOrEqual);
		mVU.profiler.EmitOp(opIBGEZ);
	}
	pass3 { mVUlog("IBGEZ vi%02d [<a href=\"#addr%04x\">%04x</a>]", _Fs_, branchAddr(mVU), branchAddr(mVU)); }
}

mVUop(mVU_IBGTZ)
{
	setBranchA(mX, 5, 0);
	pass1 { mVUanalyzeCondBranch1(mVU, _Is_); }
	pass2
	{
		if (mVUlow.memReadIs)
			xMOV(gprT1, ptr32[&mVU.VIbackup]);
		else
			mVU.regAlloc->moveVIToGPR(gprT1, _Is_);
		if (!(isBadOrEvil))
			xMOV(ptr32[&mVU.branch], gprT1);
		else
			condEvilBranch(mVU, Jcc_Greater);
		mVU.profiler.EmitOp(opIBGTZ);
	}
	pass3 { mVUlog("IBGTZ vi%02d [<a href=\"#addr%04x\">%04x</a>]", _Fs_, branchAddr(mVU), branchAddr(mVU)); }
}

mVUop(mVU_IBLEZ)
{
	setBranchA(mX, 6, 0);
	pass1 { mVUanalyzeCondBranch1(mVU, _Is_); }
	pass2
	{
		if (mVUlow.memReadIs)
			xMOV(gprT1, ptr32[&mVU.VIbackup]);
		else
			mVU.regAlloc->moveVIToGPR(gprT1, _Is_);
		if (!(isBadOrEvil))
			xMOV(ptr32[&mVU.branch], gprT1);
		else
			condEvilBranch(mVU, Jcc_LessOrEqual);
		mVU.profiler.EmitOp(opIBLEZ);
	}
	pass3 { mVUlog("IBLEZ vi%02d [<a href=\"#addr%04x\">%04x</a>]", _Fs_, branchAddr(mVU), branchAddr(mVU)); }
}

mVUop(mVU_IBLTZ)
{
	setBranchA(mX, 7, 0);
	pass1 { mVUanalyzeCondBranch1(mVU, _Is_); }
	pass2
	{
		if (mVUlow.memReadIs)
			xMOV(gprT1, ptr32[&mVU.VIbackup]);
		else
			mVU.regAlloc->moveVIToGPR(gprT1, _Is_);
		if (!(isBadOrEvil))
			xMOV(ptr32[&mVU.branch], gprT1);
		else
			condEvilBranch(mVU, Jcc_Less);
		mVU.profiler.EmitOp(opIBLTZ);
	}
	pass3 { mVUlog("IBLTZ vi%02d [<a href=\"#addr%04x\">%04x</a>]", _Fs_, branchAddr(mVU), branchAddr(mVU)); }
}

mVUop(mVU_IBNE)
{
	setBranchA(mX, 8, 0);
	pass1 { mVUanalyzeCondBranch2(mVU, _Is_, _It_); }
	pass2
	{
		if (mVUlow.memReadIs)
			xMOV(gprT1, ptr32[&mVU.VIbackup]);
		else
			mVU.regAlloc->moveVIToGPR(gprT1, _Is_);

		if (mVUlow.memReadIt)
			xXOR(gprT1, ptr32[&mVU.VIbackup]);
		else
		{
			const xRegister32& regT = mVU.regAlloc->allocGPR(_It_);
			xXOR(gprT1, regT);
			mVU.regAlloc->clearNeeded(regT);
		}

		if (!(isBadOrEvil))
			xMOV(ptr32[&mVU.branch], gprT1);
		else
			condEvilBranch(mVU, Jcc_NotEqual);
		mVU.profiler.EmitOp(opIBNE);
	}
	pass3 { mVUlog("IBNE vi%02d, vi%02d [<a href=\"#addr%04x\">%04x</a>]", _Ft_, _Fs_, branchAddr(mVU), branchAddr(mVU)); }
}

void normJumpPass2(mV)
{
	if (!mVUlow.constJump.isValid || mVUlow.evilBranch)
	{
		mVU.regAlloc->moveVIToGPR(gprT1, _Is_);
		xSHL(gprT1, 3);
		xAND(gprT1, mVU.microMemSize - 8);

		if (!mVUlow.evilBranch)
		{
			xMOV(ptr32[&mVU.branch], gprT1);
		}
		else
		{
			if(isEvilBlock)
				xMOV(ptr32[&mVU.evilevilBranch], gprT1);
			else
				xMOV(ptr32[&mVU.evilBranch], gprT1);
		}
		//If delay slot is conditional, it uses badBranch to go to its target
		if (mVUlow.badBranch)
		{
			xMOV(ptr32[&mVU.badBranch], gprT1);
		}
	}
}

mVUop(mVU_JR)
{
	mVUbranch = 9;
	pass1 { mVUanalyzeJump(mVU, _Is_, 0, false); }
	pass2
	{
		normJumpPass2(mVU);
		mVU.profiler.EmitOp(opJR);
	}
	pass3 { mVUlog("JR [vi%02d]", _Fs_); }
}

mVUop(mVU_JALR)
{
	mVUbranch = 10;
	pass1 { mVUanalyzeJump(mVU, _Is_, _It_, 1); }
	pass2
	{
		normJumpPass2(mVU);
		if (!mVUlow.evilBranch)
		{
			const xRegister32& regT = mVU.regAlloc->allocGPR(-1, _It_, mVUlow.backupVI);
			xMOV(regT, bSaveAddr);
			mVU.regAlloc->clearNeeded(regT);
		}
		if (mVUlow.evilBranch)
		{
			const xRegister32& regT = mVU.regAlloc->allocGPR(-1, _It_, mVUlow.backupVI);
			if (isEvilBlock)
			{
				xMOV(regT, ptr32[&mVU.evilBranch]);
				xADD(regT, 8);
				xSHR(regT, 3);
			}
			else
			{
				incPC(-2);
				DevCon.Warning("Linking JALR from %s branch taken/not taken target! - If game broken report to PCSX2 Team", branchSTR[mVUlow.branch & 0xf]);
				incPC(2);

				xMOV(regT, ptr32[&mVU.badBranch]);
				xADD(regT, 8);
				xSHR(regT, 3);
			}
			mVU.regAlloc->clearNeeded(regT);
		}

		mVU.profiler.EmitOp(opJALR);
	}
	pass3 { mVUlog("JALR vi%02d, [vi%02d]", _Ft_, _Fs_); }
}
