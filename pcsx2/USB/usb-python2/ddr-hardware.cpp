// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "ddr-hardware.h"

#ifdef _WIN32
#include "common/Console.h"
#include "common/DynamicLibrary.h"
#include "common/Error.h"
#include "common/FileSystem.h"
#include "common/Path.h"
#include "common/RedtapeWindows.h"
#include "bemanitools/ddrio.h"

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <process.h>
#include <thread>

namespace usb_python2
{
	namespace
	{
		bool OpenLibrary(DynamicLibrary& library, const char* name)
		{
			Error error;
			const std::string path = Path::Combine(Path::GetDirectory(FileSystem::GetProgramPath()), name);
			if (library.Open(path.c_str(), &error))
				return true;
			Console.ErrorFmt("P2IO: {}", error.GetDescription());
			return false;
		}

		template <typename T>
		bool LoadSymbol(DynamicLibrary& library, const char* name, T& function)
		{
			if (library.GetSymbol(name, &function))
				return true;
			Console.Error("P2IO: Missing DDR hardware DLL export '%s'.", name);
			return false;
		}

		void LogMessage(const char* module, const char* format, va_list args, bool warning)
		{
			char message[2048];
			std::vsnprintf(message, sizeof(message), format, args);
			if (warning)
				Console.Warning("DDRIO [%s]: %s", module, message);
			else
				Console.WriteLn("DDRIO [%s]: %s", module, message);
		}

		void LogInfo(const char* module, const char* format, ...)
		{
			va_list args;
			va_start(args, format);
			LogMessage(module, format, args, false);
			va_end(args);
		}

		void LogWarning(const char* module, const char* format, ...)
		{
			va_list args;
			va_start(args, format);
			LogMessage(module, format, args, true);
			va_end(args);
		}

		[[noreturn]] void LogFatal(const char* module, const char* format, ...)
		{
			va_list args;
			va_start(args, format);
			LogMessage(module, format, args, true);
			va_end(args);

			std::abort();
		}

		struct DLLThread
		{
			HANDLE handle = nullptr;
			~DLLThread()
			{
				if (handle)
					CloseHandle(handle);
			}
		};
		std::mutex s_thread_mutex;
		std::map<int, std::shared_ptr<DLLThread>> s_threads;

		struct ThreadContext
		{
			int (*proc)(void*);
			void* context;
		};

		unsigned __stdcall ThreadEntry(void* context)
		{
			const std::unique_ptr<ThreadContext> entry(static_cast<ThreadContext*>(context));
			return entry->proc(entry->context);
		}

		int CreateThread(int (*proc)(void*), void* context, uint32_t stack_size, unsigned int priority)
		{
			auto entry = std::make_unique<ThreadContext>(ThreadContext{proc, context});
			auto thread = std::make_shared<DLLThread>();
			std::unique_lock lock(s_thread_mutex);

			int id = 1;

			while (s_threads.contains(id))
				id++;

			s_threads.emplace(id, thread);
			thread->handle = reinterpret_cast<HANDLE>(_beginthreadex(nullptr, stack_size, ThreadEntry, entry.get(), CREATE_SUSPENDED, nullptr));

			if (!thread->handle)
			{
				s_threads.erase(id);
				return -1;
			}

			entry.release();
			(void)priority;
			ResumeThread(thread->handle);

			return id;
		}

		void JoinThread(int id, int* result)
		{
			std::shared_ptr<DLLThread> thread;
			{
				std::unique_lock lock(s_thread_mutex);
				const auto it = s_threads.find(id);

				if (it == s_threads.end())
					return;
				thread = it->second;
			}

			WaitForSingleObject(thread->handle, INFINITE);
			DWORD code = 0;

			if (result && GetExitCodeThread(thread->handle, &code))
				*result = static_cast<int>(code);
		}

		void DestroyThread(int id)
		{
			std::unique_lock lock(s_thread_mutex);
			s_threads.erase(id);
		}

		class DDRIO
		{
		public:
			DynamicLibrary library;
			decltype(&ddr_io_set_loggers) set_loggers = nullptr;
			decltype(&ddr_io_init) init = nullptr;
			decltype(&ddr_io_read_pad) read_pad = nullptr;
			decltype(&ddr_io_set_lights_p3io) set_p3io = nullptr;
			decltype(&ddr_io_set_lights_extio) set_extio = nullptr;
			decltype(&ddr_io_fini) fini = nullptr;
			std::atomic<uint32_t> pad{0}, cabinet{0}, extio{0};
			std::atomic<bool> running{false};
			std::thread worker;
			bool initialized = false;

			bool Open()
			{
				if (!OpenLibrary(library, "ddrio.dll") ||
					!LoadSymbol(library, "ddr_io_set_loggers", set_loggers) ||
					!LoadSymbol(library, "ddr_io_init", init) ||
					!LoadSymbol(library, "ddr_io_read_pad", read_pad) ||
					!LoadSymbol(library, "ddr_io_set_lights_p3io", set_p3io) ||
					!LoadSymbol(library, "ddr_io_set_lights_extio", set_extio) ||
					!LoadSymbol(library, "ddr_io_fini", fini))
					return false;

				set_loggers(LogInfo, LogInfo, LogWarning, LogFatal);
				if (!init(CreateThread, JoinThread, DestroyThread))
				{
					Console.Error("P2IO: DDRIO initialization failed.");
					return false;
				}
				initialized = true;
				running.store(true);
				worker = std::thread(&DDRIO::Poll, this);
				Console.WriteLn("P2IO: DDRIO input and lights enabled.");
				return true;
			}

			~DDRIO()
			{
				running.store(false);
				if (worker.joinable())
					worker.join();
				if (initialized)
				{
					set_p3io(0);
					set_extio(0);
					read_pad();
					fini();
				}
			}

		protected:
			void Poll()
			{
				uint32_t last_cabinet = UINT32_MAX, last_extio = UINT32_MAX;
				while (running.load())
				{
					const uint32_t new_cabinet = cabinet.load();
					const uint32_t new_extio = extio.load();
					if (new_cabinet != last_cabinet)
						set_p3io(last_cabinet = new_cabinet);
					if (new_extio != last_extio)
						set_extio(last_extio = new_extio);
					// Some backends flush lights in read_pad; keep polling even for lights-only DLLs.
					pad.store(read_pad());
					std::this_thread::sleep_for(std::chrono::milliseconds(1));
				}
			}
		};

		struct Minimaid
		{
			DynamicLibrary library;
			bool (*connect)() = nullptr;
			bool (*set_keyboard)(bool) = nullptr;
			void (*set_pad1)(int, int) = nullptr;
			void (*set_pad2)(int, int) = nullptr;
			void (*set_cabinet)(int, int) = nullptr;
			void (*set_bass)(int, int) = nullptr;
			void (*all_off)() = nullptr;
			bool (*update)() = nullptr;
			bool connected = false;

			bool Open()
			{
				if (connected)
					return true;
				if (!library.IsOpen())
				{
					if (!OpenLibrary(library, "mmmagic64.dll"))
						return false;
					if (!LoadSymbol(library, "mm_connect_minimaid", connect) ||
						!LoadSymbol(library, "mm_setKB", set_keyboard) ||
						!LoadSymbol(library, "mm_setDDRPad1Light", set_pad1) ||
						!LoadSymbol(library, "mm_setDDRPad2Light", set_pad2) ||
						!LoadSymbol(library, "mm_setDDRCabinetLight", set_cabinet) ||
						!LoadSymbol(library, "mm_setDDRBassLight", set_bass) ||
						!LoadSymbol(library, "mm_setDDRAllOff", all_off) ||
						!LoadSymbol(library, "mm_sendDDRMiniMaidUpdate", update))
					{
						library.Close();
						return false;
					}

					HMODULE pinned;
					if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
							reinterpret_cast<LPCWSTR>(connect), &pinned))
					{
						library.Close();
						return false;
					}
				}

				connected = connect();

				if (!connected)
				{
					Console.Error("P2IO: Minimaid connection failed.");
					return false;
				}

				set_keyboard(true);
				Console.WriteLn("P2IO: Minimaid lights enabled.");

				return true;
			}

			void SetLights(uint32_t cabinet, uint32_t extio)
			{
				// Minimaid's menu lamps use bits 2/3, unlike DDRIO's bits 0/1.
				set_cabinet(2, !!(cabinet & 1));
				set_cabinet(3, !!(cabinet & 2));

				for (int bit = 4; bit <= 7; bit++)
					set_cabinet(bit, !!(cabinet & (1u << bit)));

				for (int panel = 0; panel < 4; panel++)
				{
					set_pad1(panel, !!(extio & (1u << (30 - panel))));
					set_pad2(panel, !!(extio & (1u << (22 - panel))));
				}

				set_bass(0, !!(extio & (1u << 14)));
				update();
			}
		};

		Minimaid s_minimaid;

		// These ABIs expose process-global state. Only one USB device may own each backend.
		DDRHardware* s_ddrio_owner = nullptr;
		DDRHardware* s_minimaid_owner = nullptr;
	} // namespace

	struct DDRHardware::Impl
	{
		std::unique_ptr<DDRIO> ddrio;
		bool minimaid = false;
		uint32_t cabinet = 0, extio = 0;

		void Publish()
		{
			if (ddrio)
			{
				ddrio->cabinet.store(cabinet);
				ddrio->extio.store(extio);
			}
			if (minimaid)
				s_minimaid.SetLights(cabinet, extio);
		}
	};

	DDRHardware::DDRHardware()
		: m_impl(std::make_unique<Impl>())
	{
	}

	DDRHardware::~DDRHardware()
	{
		Configure(false, false);
	}

	void DDRHardware::Configure(bool ddrio, bool minimaid)
	{
		// (AI-assisted) Retry inactive backends here; cabinet handoff still needs proper testing.
		ConfigureDDRIO(ddrio);
		ConfigureMinimaid(minimaid);
		m_impl->Publish();
	}

	void DDRHardware::ConfigureDDRIO(bool enabled)
	{
		if (enabled == static_cast<bool>(m_impl->ddrio))
			return;

		if (!enabled)
		{
			m_impl->ddrio.reset();
			s_ddrio_owner = nullptr;
			return;
		}

		if (s_ddrio_owner)
		{
			Console.Warning("P2IO: DDRIO is already in use by another USB port.");
			return;
		}

		auto backend = std::make_unique<DDRIO>();
		if (!backend->Open())
			return;

		m_impl->ddrio = std::move(backend);
		s_ddrio_owner = this;
	}

	void DDRHardware::ConfigureMinimaid(bool enabled)
	{
		if (enabled == m_impl->minimaid)
			return;

		if (!enabled)
		{
			s_minimaid.all_off();
			s_minimaid.update();
			m_impl->minimaid = false;
			s_minimaid_owner = nullptr;
			return;
		}

		if (s_minimaid_owner)
		{
			Console.Warning("P2IO: Minimaid is already in use by another USB port.");
			return;
		}

		if (!s_minimaid.Open())
			return;

		m_impl->minimaid = true;
		s_minimaid_owner = this;
		s_minimaid.all_off();
	}

	void DDRHardware::ResetLights()
	{
		m_impl->cabinet = m_impl->extio = 0;
		m_impl->Publish();
	}

	void DDRHardware::SetCabinetLights(uint8_t lamps)
	{
		const uint32_t cabinet = DDRCabinetLights(lamps);

		if (m_impl->cabinet == cabinet)
			return;

		m_impl->cabinet = cabinet;
		m_impl->Publish();
	}

	void DDRHardware::SetExtioLights(uint8_t p1, uint8_t p2, uint8_t neon)
	{
		const uint32_t extio = DDRExtioLights(p1, p2, neon);

		if (m_impl->extio == extio)
			return;

		m_impl->extio = extio;
		m_impl->Publish();
	}

	uint32_t DDRHardware::GetPressedButtons() const
	{
		return m_impl->ddrio ? DDRIOToJamma(m_impl->ddrio->pad.load()) : 0;
	}
} // namespace usb_python2
#else
namespace usb_python2
{
	struct DDRHardware::Impl
	{
	};
	DDRHardware::DDRHardware() = default;
	DDRHardware::~DDRHardware() = default;
	void DDRHardware::Configure(bool, bool) {}
	void DDRHardware::ResetLights() {}
	void DDRHardware::SetCabinetLights(uint8_t) {}
	void DDRHardware::SetExtioLights(uint8_t, uint8_t, uint8_t) {}
	uint32_t DDRHardware::GetPressedButtons() const { return 0; }
} // namespace usb_python2
#endif
