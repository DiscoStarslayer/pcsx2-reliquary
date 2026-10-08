// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// Complete-pair dispatch with canonical pipeline exports at every boundary.
static void mVUemitCommunicationDispatch(microVU& mVU, std::vector<xForwardJump32>& finished)
{
	// Translate only host dispatch bookkeeping. Guest instructions, complete
	// boundaries, costs and earlier-unit/tie ordering stay unchanged. Hints are
	// consumed once and invalidated on cache reset, deletion or micro-memory writes.
	auto& r = g_mvuCommunicationRequest;
	auto& vu = mVU.regs();
	std::vector<xForwardJump32> slow;
	std::vector<xForwardJump32> completed;
	const auto fail = [&slow](JccComparisonType cc) { slow.emplace_back(cc); };
	xCMP(ptr32[&r.active], 0);
	fail(Jcc_Zero);
	xTEST(ptr32[&vu.flags], VUFLAG_INTCINTERRUPT);
	fail(Jcc_NotZero);
	xMOV(rax, ptr64[&vu.cycle]);
	xCMP(rax, ptr64[&r.before]);
	fail(Jcc_BelowOrEqual);

	xCMP(ptr32[&r.requestor], 0);
	xForwardJump32 request1(Jcc_NotEqual);
	xTEST(ptr32[&VU0.VI[REG_VPU_STAT].UL], 1);
	completed.emplace_back(Jcc_Zero);
	xTEST(ptr32[&VU0.flags], VUFLAG_MFLAGSET);
	completed.emplace_back(Jcc_NotZero);
	xMOV(rax, ptr64[&VU0.cycle]);
	xCMP(rax, ptr64[&r.target]);
	completed.emplace_back(Jcc_AboveOrEqual);
	xForwardJump32 checked(Jcc_Unconditional);
	request1.SetTarget();
	xTEST(ptr32[&VU0.VI[REG_VPU_STAT].UL], 0x100);
	completed.emplace_back(Jcc_Zero);
	xMOV(rax, ptr64[&VU1.cycle]);
	xCMP(rax, ptr64[&r.target]);
	completed.emplace_back(Jcc_AboveOrEqual);
	checked.SetTarget();
	xTEST(ptr32[&VU0.VI[REG_VPU_STAT].UL], 1);
	xForwardJump32 select1Inactive0(Jcc_Zero);
	xTEST(ptr32[&VU0.VI[REG_VPU_STAT].UL], 0x100);
	xForwardJump32 select0Inactive1(Jcc_Zero);
	xMOV(rax, ptr64[&VU0.cycle]);
	xCMP(rax, ptr64[&VU1.cycle]);
	xForwardJump32 select1Behind(Jcc_Above); // tie selects VU0
	select0Inactive1.SetTarget();
	const auto emit = [&](microVU& next) {
		const bool same = next.index == mVU.index;
		auto& nvu = next.regs();
		auto& hint = g_mvuCommunicationHint[next.index];
		xMOV(rax, ptrNative[&hint.block]);
		xTEST(rax, rax);
		fail(Jcc_Zero);
		xMOV(edx, ptr32[&nvu.VI[REG_TPC].UL]);
		if (!same)
			xSHL(edx, 3);
		xCMP(edx, ptr32[&hint.pc]);
		fail(Jcc_NotEqual);
		xCMP(ptr32[&next.prog.cleared], 0);
		fail(Jcc_NotEqual);
		xMOV(r8, ptrNative[&hint.program]);
		xCMP(r8, ptrNative[&next.prog.cur]);
		fail(Jcc_NotEqual);
		// No EE interval and no writable micro-memory/pipeline between this hint
		// and admission. Reset/clear nulls it; full comparisons apply across EE callers.
		xCMP(ptr32[rax + offsetof(microBlock, cycles)], 0);
		fail(Jcc_Zero);
		if (!same)
			xSHR(ptr32[&vu.VI[REG_TPC].UL], 3);
		xMOV(ptr32[&nvu.VI[REG_TPC].UL], edx);
		xMOV(ptr32[&r.pc], edx);
		xMOV(ptr32[&r.unit], next.index);
		xMOV(r8, ptr64[&nvu.cycle]);
		xMOV(ptr64[&r.before], r8);
		xMOV(ecx, ptr32[rax + offsetof(microBlock, cycles)]);

		xMOV(ptrNative[&hint.block], 0);
		xMOV(ptr32[&next.prog.isSame], -1);
		if (!next.index)
			xAND(ptr32[&VU0.flags], ~VUFLAG_MFLAGSET);
		xMOV(rax, ptrNative[rax + offsetof(microBlock, x86ptrStart)]);

		// Entry and cycle counters are already resolved. Reload canonical guest
		// flags/PQ through the same body without repeating argument/lookup work.
		xMOV(ptr32[&r.runCycles], ecx);
		xMOV(ptr32[&next.cycles], ecx);
		xMOV(ptr32[&next.totalCycles], ecx);
		xMOV(r8, ptrNative[&next.prog.x86ptr]);
		xMOV(ptrNative[&x86Ptr], r8);
		pxAssert(microVU0.textPtr() == microVU1.textPtr());
		xJMP(ptrNative[&g_mvuCommunicationHotBody[next.index]]);
	};
	emit(microVU0);
	select1Inactive0.SetTarget();
	select1Behind.SetTarget();
	emit(microVU1);
	// Return and handoff share the same progress/request checks. Every guest
	// export precedes this decision; only duplicate host guards are removed.
	for (auto& jump : completed)
		jump.SetTarget();
	xSHR(ptr32[&vu.VI[REG_TPC].UL], 3);
	finished.emplace_back(Jcc_Unconditional);
	for (auto& jump : slow)
		jump.SetTarget();
}
