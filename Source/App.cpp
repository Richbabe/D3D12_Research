#include "stdafx.h"
#include "App.h"

#include "Core/Input.h"
#include "Core/Console.h"
#include "Core/CommandLine.h"
#include "Core/TaskQueue.h"
#include "Core/ConsoleVariables.h"
#include "Core/Window.h"
#include "Core/Profiler.h"

#include "RHI/Device.h"
#include "RHI/CommandQueue.h"
#include "RHI/CommandContext.h"

#include "Renderer/RenderTypes.h"
#include "Renderer/Techniques/ImGuiRenderer.h"
#include "RenderGraph/RenderGraphAllocator.h"

#ifdef _DEBUG
#define _CRTDBG_MAP_ALLOC
#include <crtdbg.h>
#endif

#define BREAK_ON_ALLOC 0

#ifdef LIVE_PP_PATH
#include "LPP_API_x64_CPP.h"
struct LivePPAgent
{
	LivePPAgent()
	{
		// create a default agent, loading the Live++ agent from the given path, e.g. "ThirdParty/LivePP"
		Agent = lpp::LppCreateDefaultAgent(nullptr, LIVE_PP_PATH);
		// bail out in case the agent is not valid
		if (lpp::LppIsValidDefaultAgent(&Agent))
		{
			// enable Live++ for all loaded modules
			Agent.EnableModule(lpp::LppGetCurrentModulePath(), lpp::LPP_MODULES_OPTION_ALL_IMPORT_MODULES, nullptr, nullptr);
		}
	}

	~LivePPAgent()
	{
		if (lpp::LppIsValidDefaultAgent(&Agent))
		{
			// destroy the Live++ agent
			lpp::LppDestroyDefaultAgent(&Agent);
		}
	}

	lpp::LppDefaultAgent Agent;
};
static LivePPAgent sLivePPAgent;
#endif

namespace Tweakables
{
	ConsoleVariable gLimitFPS("app.LimitFPS", true);
	ConsoleVariable gMaxFPS("app.MaxFPS", 60);
}

// Throttle to the target frame rate. A high resolution waitable timer lands within a fraction of a
// millisecond of the deadline, where Sleep() would overshoot by the ~15ms scheduler tick.
static void LimitFrameRate()
{
	static const int64 frequency = []
		{
			LARGE_INTEGER value;
			QueryPerformanceFrequency(&value);
			return value.QuadPart;
		}();

	// Tracked as an absolute deadline rather than a per-frame delay so that frames finishing early or
	// late don't make the pace drift.
	static int64 nextFrameTicks = 0;

	const int maxFPS = Tweakables::gMaxFPS;
	if (!Tweakables::gLimitFPS || maxFPS <= 0)
	{
		nextFrameTicks = 0;
		return;
	}

	LARGE_INTEGER now;
	QueryPerformanceCounter(&now);
	const int64 frameTicks = frequency / maxFPS;

	// Advance by exactly one frame so the pace stays anchored to a fixed grid rather than drifting by
	// however long each frame happened to take.
	nextFrameTicks = nextFrameTicks == 0 ? now.QuadPart : nextFrameTicks + frameTicks;

	// The deadline has already passed, so the frame ran longer than the target rate allows. Resync to
	// now and don't sleep at all: sleeping here would add a full period on top of an already slow frame,
	// and carrying the deficit forward would stall the frames after it.
	if (nextFrameTicks <= now.QuadPart)
	{
		nextFrameTicks = now.QuadPart;
		return;
	}

	const int64 waitTicks = nextFrameTicks - now.QuadPart;

	static HANDLE timer = []
		{
			HANDLE handle = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
			if (!handle)
				handle = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
			return handle;
		}();

	if (timer)
	{
		LARGE_INTEGER dueTime;
		dueTime.QuadPart = -(waitTicks * 10'000'000 / frequency);	// Negative is a relative time, in 100ns units
		if (SetWaitableTimer(timer, &dueTime, 0, nullptr, nullptr, FALSE))
			WaitForSingleObject(timer, INFINITE);
	}
}

#ifdef SUPERLUMINAL_PATH
#include "Superluminal/PerformanceAPI_capi.h"
#include "Superluminal/PerformanceAPI_loader.h"
struct SuperluminalAPI
{
	SuperluminalAPI()
	{
		Module = PerformanceAPI_LoadFrom(SUPERLUMINAL_PATH, &Functions);
	}
	~SuperluminalAPI()
	{
		PerformanceAPI_Free(&Module);
	}

	PerformanceAPI_ModuleHandle Module;
	PerformanceAPI_Functions Functions;
};
static SuperluminalAPI sSuperluminal;
#endif


App::App() = default;
App::~App() = default;

int App::Run()
{
	Init_Internal();
	while (m_Window.PollMessages())
	{
		PROFILE_FRAME();

		Update_Internal();
	}
	Shutdown_Internal();
	return 0;
}


static void InitializeProfiler(GraphicsDevice* pDevice)
{
	const uint32 frameHistory = 8;
	// Loading a large scene records a lot of events in a single frame, so all budgets are generous.
	// The GPU profiler packs event indices in 16 bits, capping the two GPU budgets at 65535 combined.
	const uint32 maxCPUEvents = 65535;
	const uint32 maxGPUEvents = 30000;
	const uint32 maxGPUCopyEvents = 2000;
	const uint32 maxGPUActiveCmdLists = 64;

	gCPUProfiler.Initialize(frameHistory, maxCPUEvents);

	CPUProfilerCallbacks cpuCallbacks;
	cpuCallbacks.OnEventBegin = [](const char* pName, void*)
		{
#if ENABLE_PIX
			::PIXBeginEvent(0, MULTIBYTE_TO_UNICODE(pName));
#endif
#ifdef SUPERLUMINAL_PATH
			sSuperluminal.Functions.BeginEvent(pName, nullptr, 0xFFFFFFFF);
#endif
		};
	cpuCallbacks.OnEventEnd = [](void*)
		{
#if ENABLE_PIX
			::PIXEndEvent();
#endif
#ifdef SUPERLUMINAL_PATH
			sSuperluminal.Functions.EndEvent();
#endif
		};
	gCPUProfiler.SetEventCallback(cpuCallbacks);

	ID3D12CommandQueue* pQueues[] =
	{
		pDevice->GetGraphicsQueue()->GetCommandQueue(),
		pDevice->GetComputeQueue()->GetCommandQueue(),
		pDevice->GetCopyQueue()->GetCommandQueue(),
	};
	gGPUProfiler.Initialize(pDevice->GetDevice(), pQueues, frameHistory, 3, maxGPUEvents, maxGPUCopyEvents, maxGPUActiveCmdLists);

#if ENABLE_PIX
	GPUProfilerCallbacks gpuCallbacks;
	gpuCallbacks.OnEventBegin = [](const char* pName, ID3D12GraphicsCommandList* pCmd, void*) {	::PIXBeginEvent(pCmd, 0, MULTIBYTE_TO_UNICODE(pName)); };
	gpuCallbacks.OnEventEnd = [](ID3D12GraphicsCommandList* pCmd, void*) { ::PIXEndEvent(pCmd);	};
	gGPUProfiler.SetEventCallback(gpuCallbacks);
#endif

	PROFILE_REGISTER_THREAD("Main Thread");
}

void App::Init_Internal()
{
#ifdef _DEBUG
	_CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
#if BREAK_ON_ALLOC > 0
	_CrtSetBreakAlloc(BREAK_ON_ALLOC);
#endif
#endif

	Thread::SetMainThread();
	CommandLine::Parse(GetCommandLineA());

	if (CommandLine::GetBool("debuggerwait"))
	{
		while (!::IsDebuggerPresent())
		{
			::Sleep(100);
		}
	}

	Console::Initialize();
	ConsoleManager::Initialize();

	TaskQueue::Initialize(std::thread::hardware_concurrency());

	Vector2i displayDimensions = Window::GetDisplaySize();

	m_Window.Init((int)(displayDimensions.x * 0.7f), (int)(displayDimensions.y * 0.7f));
	m_Window.OnKeyInput			+= [](uint32 character, bool isDown)	{ Input::Instance().UpdateKey(character, isDown); };
	m_Window.OnMouseInput		+= [](uint32 mouse, bool isDown)		{ Input::Instance().UpdateMouseKey(mouse, isDown); };
	m_Window.OnMouseMove		+= [](uint32 x, uint32 y)				{ Input::Instance().UpdateMousePosition((float)x, (float)y); };
	m_Window.OnMouseScroll		+= [](float wheel)						{ Input::Instance().UpdateMouseWheel(wheel); };
	m_Window.OnResizeOrMove		+= [this](uint32 width, uint32 height)	{ OnWindowResizeOrMove(width, height); };
	m_Window.SetTitle("App");

	Time::Reset();

	E_LOG(Info, "Graphics::InitD3D()");

	GraphicsDeviceOptions options;
	options.UseDebugDevice		= CommandLine::GetBool("d3ddebug");
	options.UseDRED				= CommandLine::GetBool("dred");
	options.LoadPIX				= CommandLine::GetBool("pix");
	options.UseGPUValidation	= CommandLine::GetBool("gpuvalidation");
	options.UseWarp				= CommandLine::GetBool("warp");
	options.UseStablePowerState = CommandLine::GetBool("stablepowerstate");
	m_pDevice = new GraphicsDevice(options);

	InitializeProfiler(m_pDevice);

	m_pSwapchain = new SwapChain(m_pDevice, DisplayMode::SDR, 3, m_Window.GetNativeWindow());
	gRenderGraphAllocator.Init(m_pDevice);

	GraphicsCommon::Create(m_pDevice);

	ImGuiRenderer::Initialize(m_pDevice, m_Window.GetNativeWindow());

	Init();
}

void App::Update_Internal()
{
	Time::Tick();
	ImGuiRenderer::NewFrame();

	m_pDevice->GetShaderManager()->ConditionallyReloadShaders();
	gRenderGraphAllocator.Tick();

	Update();
	Input::Instance().Update();

	{
		PROFILE_CPU_SCOPE("Execute Commandlist");
		CommandContext* pContext = m_pDevice->AllocateCommandContext();
		ImGuiRenderer::Render(*pContext, m_pSwapchain->GetBackBuffer());
		pContext->InsertResourceBarrier(m_pSwapchain->GetBackBuffer(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
		m_pDevice->GetGraphicsQueue()->ExecuteCommandLists(pContext);
	}

	{
		PROFILE_CPU_SCOPE("Present");
		m_pSwapchain->Present();
		ImGuiRenderer::PresentViewports();
	}
	{
		PROFILE_CPU_SCOPE("Wait for GPU frame");
		m_pDevice->TickFrame();
	}
	{
		PROFILE_CPU_SCOPE("Frame Limiter");
		LimitFrameRate();
	}
}

void App::Shutdown_Internal()
{
	Shutdown();

	m_pDevice->IdleGPU();
	gGPUProfiler.Shutdown();
	gCPUProfiler.Shutdown();

	gRenderGraphAllocator.Shutdown();
	ImGuiRenderer::Shutdown();
	GraphicsCommon::Destroy();

	TaskQueue::Shutdown();
	Console::Shutdown();
}

void App::OnWindowResizeOrMove(uint32 width, uint32 height)
{
	E_LOG(Info, "Window resized: %dx%d", width, height);
	m_pSwapchain->OnResizeOrMove(width, height);
}
