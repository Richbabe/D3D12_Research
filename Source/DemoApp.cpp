#include "stdafx.h"
#include "DemoApp.h"

#include "Core/CommandLine.h"
#include "Core/Profiler.h"
#include "Core/ConsoleVariables.h"
#include "Core/Paths.h"
#include "Core/Json.h"

#include "Renderer/Renderer.h"
#include "Renderer/Techniques/DDGI.h"
#include "Renderer/Techniques/CBTTessellation.h"
#include "Renderer/Techniques/DebugRenderer.h"
#include "Renderer/Mesh.h"
#include "Renderer/Light.h"

#include "Scene/SceneLoader.h"
#include "Scene/Camera.h"

#include <imgui_internal.h>
#include <IconsFontAwesome4.h>
#include <ImGuizmo.h>

bool sScreenshotNextFrame = false;

namespace Tweakables
{
	// Frame pacing, owned by App
	extern ConsoleVariable<bool> gLimitFPS;
	extern ConsoleVariable<int> gMaxFPS;
}

struct SceneDescription
{
	const char* pName;
	const char* pPath;
	// Optional scene description holding the lighting rig. Scenes without one get a default sun and spotlights.
	const char* pLightsPath;
	Vector3 CameraPosition;
	float CameraYaw;
	float CameraPitch;
	float ViewDistance;
};

// Paths are relative to the resources directory. The first entry is loaded on startup and
// doubles as the fallback camera setup for meshes loaded through the file dialog.
static const SceneDescription gScenes[] = {
	{ "Sponza",	"Scenes/Sponza/Sponza.gltf",	nullptr,									Vector3(5.80f, 0.96f, 0.25f),		Math::Radians(-89.72f),	Math::Radians(0.73f),	80.0f },
	{ "Bistro",	"Scenes/Bistro/Bistro.gltf",	"Scenes/Bistro/bistro-rtxdi.scene.json",	Vector3(-25.36f, 2.74f, -11.64f),	Math::Radians(85.12f),	Math::Radians(-0.18f),	300.0f },
};

static String ResolveScenePath(const char* pPath)
{
	return Paths::Normalize(Paths::MakeAbsolute(pPath));
}

static String GetScenePath(const SceneDescription& scene)
{
	return ResolveScenePath((Paths::ResourcesDir() + scene.pPath).c_str());
}

// The renderer indexes World::Sunlight directly, so every scene needs exactly one directional light.
static entt::entity CreateSunlight(World& world, const Quaternion& rotation)
{
	entt::entity entity = world.CreateEntity("Sunlight");
	Transform& transform = world.Registry.emplace<Transform>(entity);
	transform.Rotation = rotation;

	Light& light = world.Registry.emplace<Light>(entity);
	light.Type = LightType::Directional;
	light.Intensity = 5;
	light.Colour = Math::MakeFromColorTemperature(5900);
	light.CastShadows = true;
	light.VolumetricLighting = true;

	world.Sunlight = entity;
	return entity;
}

// Reads the lighting rig out of a Donut scene description, the format NVIDIA ships the RTXDI assets in.
// Only the 'Lights' subtree is used; models, animations and probe volumes are ignored.
// Returns false when the file is missing or holds no usable lights, leaving the world untouched.
static bool LoadSceneLights(const char* pFilePath, World& world)
{
	Json::Value root;
	if (!Json::ParseFile(pFilePath, root))
	{
		E_LOG(Warning, "Scene lights - Failed to parse '%s'", pFilePath);
		return false;
	}

	const Json::Value* pLightNodes = nullptr;
	const Json::Value& graph = root["graph"];
	for (uint32 i = 0; i < graph.GetSize(); ++i)
	{
		if (strcmp(graph[i]["name"].GetString(), "Lights") == 0)
			pLightNodes = &graph[i]["children"];
	}
	if (!pLightNodes || pLightNodes->GetSize() == 0)
	{
		E_LOG(Warning, "Scene lights - '%s' has no 'Lights' group", pFilePath);
		return false;
	}

	// The glTF loader mirrors Z to get from the source's right-handed space into the engine's
	// left-handed one, so anything authored alongside it has to go through the same flip.
	auto MirrorZ = [](const Vector3& v) { return Vector3(v.x, v.y, -v.z); };

	uint32 numLights = 0;
	bool hasSun = false;

	for (uint32 i = 0; i < pLightNodes->GetSize(); ++i)
	{
		const Json::Value& node = (*pLightNodes)[i];

		Vector3 position = Vector3::Zero;
		const Json::Value& translation = node["translation"];
		if (translation.GetSize() == 3)
			position = MirrorZ(Vector3(translation[0].GetFloat(), translation[1].GetFloat(), translation[2].GetFloat()));

		// Lights shine down their local -Z. Resolving that to a world direction before mirroring is
		// less error-prone than trying to mirror the quaternion itself.
		Quaternion sourceRotation = Quaternion::Identity;
		const Json::Value& rotation = node["rotation"];
		if (rotation.GetSize() == 4)
			sourceRotation = Quaternion(rotation[0].GetFloat(), rotation[1].GetFloat(), rotation[2].GetFloat(), rotation[3].GetFloat());
		Vector3 direction = MirrorZ(Vector3::Transform(Vector3(0, 0, -1), sourceRotation));
		Quaternion worldRotation = Quaternion::LookRotation(direction, Vector3::Up);

		const char* pType = node["type"].GetString();
		if (strcmp(pType, "DirectionalLight") == 0)
		{
			if (hasSun)
				continue;
			CreateSunlight(world, worldRotation);
			hasSun = true;
			++numLights;
			continue;
		}

		Light light;
		if (strcmp(pType, "SpotLight") == 0)
		{
			light.Type = LightType::Spot;
			// Donut measures the cone from its axis in degrees, the engine stores the full cone angle in radians.
			light.InnerConeAngle = 2.0f * Math::Radians(node["innerAngle"].GetFloat(30.0f));
			light.OuterConeAngle = 2.0f * Math::Radians(node["outerAngle"].GetFloat(45.0f));
		}
		else if (strcmp(pType, "PointLight") == 0)
		{
			light.Type = LightType::Point;
		}
		else
		{
			continue;
		}

		light.Intensity = node["intensity"].GetFloat(1.0f);
		light.Range = node["range"].GetFloat(10.0f);
		// These rigs are authored as many-light stress tests, so every light casting shadows would
		// add a shadow map and a full GPU-culled raster pipeline each. Only the sun does by default.
		light.CastShadows = false;
		light.VolumetricLighting = true;

		const Json::Value& color = node["color"];
		if (color.GetSize() == 3)
			light.Colour = Color(color[0].GetFloat(), color[1].GetFloat(), color[2].GetFloat(), 1.0f);

		entt::entity entity = world.CreateEntity(node["name"].GetString("Light"));
		Transform& transform = world.Registry.emplace<Transform>(entity);
		transform.Position = position;
		transform.Rotation = worldRotation;
		world.Registry.emplace<Light>(entity, light);
		++numLights;
	}

	if (numLights == 0)
		return false;

	if (!hasSun)
		CreateSunlight(world, Quaternion::CreateFromYawPitchRoll(Math::PI / 3, Math::PI_DIV_4, 0));

	E_LOG(Info, "Scene lights - Loaded %d lights from '%s'", numLights, pFilePath);
	return true;
}

DemoApp::DemoApp() = default;

DemoApp::~DemoApp() = default;

void DemoApp::Init()
{
	String scenePath = GetScenePath(gScenes[0]);
	const char* pScene = scenePath.c_str();
	CommandLine::GetValue("scene", &pScene);
	SetupScene(pScene);

	m_Renderer.Init(m_pDevice, &m_World);
}

void DemoApp::Update()
{
	DrawImGui();

	Camera& camera = m_World.GetComponent<Camera>(m_World.Camera);
	Transform& cameraTransform = m_World.GetComponent<Transform>(m_World.Camera);
	Camera::UpdateMovement(cameraTransform, camera);

	{
		PROFILE_CPU_SCOPE("Update Entity Transforms");
		auto view = m_World.Registry.view<Transform>();
		view.each([&](Transform& transform)
			{
				transform.WorldPrev = transform.World;
				transform.World = Matrix::CreateScale(transform.Scale) *
					Matrix::CreateFromQuaternion(transform.Rotation) *
					Matrix::CreateTranslation(transform.Position);
			});
	}

	if (m_pViewportTexture)
	{
		m_Renderer.Render(cameraTransform, camera, m_pViewportTexture);

		if (sScreenshotNextFrame)
		{
			sScreenshotNextFrame = false;
			m_Renderer.MakeScreenshot(m_pViewportTexture);
		}
	}
}

void DemoApp::Shutdown()
{
	m_Renderer.Shutdown();
}

void DemoApp::SetupScene(const char* pFilePath)
{
	m_World = {};
	m_ScenePath = ResolveScenePath(pFilePath);

	const SceneDescription* pScene = &gScenes[0];
	for (const SceneDescription& scene : gScenes)
	{
		if (GetScenePath(scene) == m_ScenePath)
			pScene = &scene;
	}

	Material& defaultMaterial = m_World.Materials.emplace_back();
	defaultMaterial.BaseColorFactor = Vector4(0.7f, 0.7f, 0.7f, 1.0f);

	m_World.Camera = m_World.CreateEntity("Main Camera");
	Camera& camera = m_World.Registry.emplace<Camera>(m_World.Camera);
	camera.FOV = 60.0f * Math::PI / 180;
	Transform& cameraTransform = m_World.Registry.emplace<Transform>(m_World.Camera);
	cameraTransform.Position = pScene->CameraPosition;
	cameraTransform.Rotation = Quaternion::CreateFromYawPitchRoll(pScene->CameraYaw, pScene->CameraPitch, 0);
	m_Renderer.SetViewDistance(pScene->ViewDistance);

	SceneLoader::Load(pFilePath, m_pDevice, m_World);

	// The built-in rig below is sized for Sponza's atrium, so it's only used for scenes that don't ship their own.
	bool hasSceneLights = false;
	if (pScene->pLightsPath)
		hasSceneLights = LoadSceneLights((Paths::ResourcesDir() + pScene->pLightsPath).c_str(), m_World);

	if (!hasSceneLights)
	{
		CreateSunlight(m_World, Quaternion::CreateFromYawPitchRoll(Math::PI / 3, Math::PI_DIV_4, 0));

		Light spot;
		spot.Range = 4;
		spot.OuterConeAngle = 70.0f * Math::DegreesToRadians;
		spot.InnerConeAngle = 50.0f * Math::DegreesToRadians;
		spot.Intensity = 100.0f;
		spot.CastShadows = true;
		spot.VolumetricLighting = true;
		spot.pLightTexture = GraphicsCommon::CreateTextureFromFile(m_pDevice, (Paths::ResourcesDir() + "Textures/LightProjector.png").c_str(), false, "Light Cookie");
		spot.Type = LightType::Spot;

		Vector3 positions[] = {
			Vector3(9.5, 3, 3.5),
			Vector3(-9.5, 3, 3.5),
			Vector3(9.5, 3, -3.5),
			Vector3(-9.5, 3, -3.5),
		};

		for (Vector3 v : positions)
		{
			entt::entity entity = m_World.CreateEntity("Spotlight");
			Transform& transform = m_World.Registry.emplace<Transform>(entity);
			transform.Rotation = Quaternion::LookRotation(Vector3::Down, Vector3::Right);
			transform.Position = v;
			m_World.Registry.emplace<Light>(entity, spot);
		}
	}
	{
		entt::entity entity = m_World.CreateEntity("DDGI Volume");
		Transform& transform = m_World.Registry.emplace<Transform>(entity);
		transform.Position = Vector3(-0.484151840f, 5.21196413f, 0.309524536f);

		DDGIVolume& volume = m_World.Registry.emplace<DDGIVolume>(entity);
		volume.Extents = Vector3(14.8834171f, 6.22350454f, 9.15293312f);
		volume.NumProbes = Vector3i(16, 12, 14);
		volume.NumRays = 128;
		volume.MaxNumRays = 512;
	}

	{
		entt::entity entity = m_World.CreateEntity("Fog Volume");
		Transform& transform = m_World.Registry.emplace<Transform>(entity);
		transform.Position = Vector3(0, 1, 0);

		FogVolume& volume = m_World.Registry.emplace<FogVolume>(entity);
		volume.Extents = Vector3(100, 100, 100);
		volume.Color = Vector3(1, 1, 1);
		volume.DensityBase = 0;
		volume.DensityChange = 0.03f;
	}

	{
		entt::entity entity = m_World.CreateEntity("Terrain");
		CBTData cbtData = m_World.Registry.emplace<CBTData>(entity);
	}

	auto ddgi_view = m_World.Registry.view<Transform, DDGIVolume>();
}


void DemoApp::DrawImGui()
{
	static ImGuiConsole console;
	static bool showProfiler = false;
	static bool showStats = true;
	static bool showImguiDemo = false;
	static bool showToolMetrics = false;

	if (showImguiDemo)
		ImGui::ShowDemoWindow();

	if (ImGui::IsKeyDown(ImGuiKey_LeftCtrl) && ImGui::IsKeyPressed(ImGuiKey_P))
		showProfiler = !showProfiler;

	ImGuiViewport* pViewport = ImGui::GetMainViewport();
	ImGuiID dockspace = ImGui::DockSpaceOverViewport(ImGui::GetID("Dockspace"), pViewport);

	if (!ImGui::FindWindowSettingsByID(ImHashStr("ViewportSettings")))
	{
		ImGui::CreateNewWindowSettings("ViewportSettings");
		ImGuiID viewportID, parametersID;
		ImGui::DockBuilderRemoveNode(dockspace);
		ImGui::DockBuilderAddNode(dockspace, ImGuiDockNodeFlags_CentralNode);
		ImGui::DockBuilderSetNodeSize(dockspace, pViewport->Size);
		ImGui::DockBuilderSplitNode(dockspace, ImGuiDir_Right, 0.2f, &parametersID, &viewportID);
		ImGui::DockBuilderDockWindow("Settings", parametersID);
		ImGui::DockBuilderGetNode(viewportID)->LocalFlags |= ImGuiDockNodeFlags_HiddenTabBar;
		ImGui::DockBuilderGetNode(viewportID)->UpdateMergedFlags();
		ImGui::DockBuilderDockWindow(ICON_FA_DESKTOP " Viewport", viewportID);
		ImGui::DockBuilderFinish(dockspace);
	}

	console.Update();

	if (ImGui::BeginMainMenuBar())
	{
		if (ImGui::BeginMenu(ICON_FA_FILE " File"))
		{
			if (ImGui::BeginMenu(ICON_FA_GLOBE " Load Scene"))
			{
				for (const SceneDescription& scene : gScenes)
				{
					String path = GetScenePath(scene);
					bool isAvailable = Paths::FileExists(path.c_str());
					if (ImGui::MenuItem(scene.pName, nullptr, m_ScenePath == path, isAvailable))
					{
						SetupScene(path.c_str());
					}
					if (!isAvailable && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
					{
						ImGui::SetTooltip("Not found: %s", path.c_str());
					}
				}
				ImGui::EndMenu();
			}
			if (ImGui::MenuItem(ICON_FA_FILE " Load Mesh", nullptr, nullptr))
			{
				OPENFILENAME ofn{};
				TCHAR szFile[260]{};
				ofn.lStructSize = sizeof(ofn);
				ofn.hwndOwner = m_Window.GetNativeWindow();
				ofn.lpstrFile = szFile;
				ofn.nMaxFile = sizeof(szFile);
				ofn.lpstrFilter = "Supported files (*.gltf;*.glb;*.dat;*.ldr;*.mpd)\0*.gltf;*.glb;*.dat;*.ldr;*.mpd\0All Files (*.*)\0*.*\0";;
				ofn.nFilterIndex = 1;
				ofn.lpstrFileTitle = NULL;
				ofn.nMaxFileTitle = 0;
				ofn.lpstrInitialDir = NULL;
				ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;

				if (GetOpenFileNameA(&ofn) == TRUE)
				{
					SetupScene(ofn.lpstrFile);
				}
			}
			ImGui::EndMenu();
		}
		if (ImGui::BeginMenu(ICON_FA_WINDOW_MAXIMIZE " Windows"))
		{
			if (ImGui::MenuItem(ICON_FA_CLOCK_O " Profiler", "Ctrl + P", showProfiler))
				showProfiler = !showProfiler;

			if (ImGui::MenuItem(ICON_FA_TACHOMETER " Stats", nullptr, showStats))
				showStats = !showStats;
			
			if (ImGui::MenuItem("ImGui Metrics"))
				showToolMetrics = !showToolMetrics;

			bool& showConsole = console.IsVisible();
			if (ImGui::MenuItem("Output Log", "~", showConsole))
				showConsole = !showConsole;

			ImGui::EndMenu();
		}
		if (ImGui::BeginMenu(ICON_FA_WRENCH " Tools"))
		{
			if (ImGui::MenuItem("Screenshot"))
			{
				sScreenshotNextFrame = true;
			}
			ImGui::EndMenu();
		}
		if (ImGui::BeginMenu(ICON_FA_QUESTION " Help"))
		{
			if (ImGui::MenuItem("ImGui Demo", 0, showImguiDemo))
			{
				showImguiDemo = !showImguiDemo;
			}
			ImGui::EndMenu();
		}
		ImGui::EndMainMenuBar();
	}

	if (showToolMetrics)
		ImGui::ShowMetricsWindow(&showToolMetrics);

	ImGui::Begin(ICON_FA_DESKTOP " Viewport", 0, ImGuiWindowFlags_NoScrollbar);
	ImVec2 viewportPos = ImGui::GetWindowPos();
	ImVec2 viewportSize = ImGui::GetWindowSize();
	FloatRect viewport(viewportPos.x, viewportPos.y, viewportPos.x + viewportSize.x, viewportPos.y + viewportSize.y);
	ImVec2 imageSize = ImMax(ImGui::GetContentRegionAvail(), ImVec2(16.0f, 16.0f));
	if (!m_pViewportTexture || imageSize.x != m_pViewportTexture->GetWidth() || imageSize.y != m_pViewportTexture->GetHeight())
	{
		m_pViewportTexture = m_pDevice->CreateTexture(TextureDesc::Create2D((uint32)imageSize.x, (uint32)imageSize.y, ResourceFormat::RGBA8_UNORM, 1, TextureFlag::ShaderResource | TextureFlag::UnorderedAccess), "Viewport");
	}
	ImGui::Image((ImTextureID)m_pViewportTexture.Get(), imageSize);
	ImVec2 viewportOrigin = ImGui::GetItemRectMin();
	ImVec2 viewportExtents = ImGui::GetItemRectSize();
	ImGuizmo::SetDrawlist(ImGui::GetWindowDrawList());

	ImGui::End();

	ImGuizmo::SetRect(viewportOrigin.x, viewportOrigin.y, viewportExtents.x, viewportExtents.y);
	DrawOutliner();

	if (showProfiler)
	{
		PROFILE_CPU_SCOPE("Profiler");
		if (ImGui::Begin("Profiler", &showProfiler))
		{
			DrawProfilerHUD();
		}
		ImGui::End();
	}
	else if (showStats)
	{
		// The stats are read back from the profilers, so they have to keep sampling.
		gCPUProfiler.SetPaused(false);
		gGPUProfiler.SetPaused(false);
	}
	else
	{
		gCPUProfiler.SetPaused(true);
		gGPUProfiler.SetPaused(true);
	}

	if (showStats)
	{
		if (ImGui::Begin(ICON_FA_TACHOMETER " Stats", &showStats))
		{
			DrawProfilerStats();
		}
		ImGui::End();
	}

	if (ImGui::Begin("Settings"))
	{
		if (ImGui::CollapsingHeader("Swapchain"))
		{
			bool vsync = m_pSwapchain->GetVSync();
			if (ImGui::Checkbox("Vertical Sync", &vsync))
				m_pSwapchain->SetVSync(vsync);

			ImGui::Checkbox("Limit FPS", &Tweakables::gLimitFPS.Get());
			ImGui::BeginDisabled(!Tweakables::gLimitFPS);
			ImGui::SliderInt("Max FPS", &Tweakables::gMaxFPS.Get(), 10, 240);
			ImGui::EndDisabled();
			if (vsync)
				ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.4f, 1.0f), "V-Sync caps the frame rate to the display refresh rate");

			int swapchainFrames = m_pSwapchain->GetNumFrames();
			if (ImGui::SliderInt("Swapchain Frames", &swapchainFrames, 2, 5))
				m_pSwapchain->SetNumFrames(swapchainFrames);
			bool waitableSwapChain = m_pSwapchain->GetUseWaitableSwapChain();
			if (ImGui::Checkbox("Waitable Swapchain", &waitableSwapChain))
				m_pSwapchain->SetUseWaitableSwapChain(waitableSwapChain);
			int frameLatency = m_pSwapchain->GetMaxFrameLatency();
			if (ImGui::SliderInt("Max Frame Latency", &frameLatency, 1, 5))
				m_pSwapchain->SetMaxFrameLatency(frameLatency);
		}
	}
	ImGui::End();

	m_Renderer.DrawImGui();
}


void DemoApp::DrawOutliner()
{
	static entt::entity selectedEntity = entt::null;
	if (ImGui::Begin("Outliner"))
	{
		auto entity_view = m_World.Registry.view<Identity>();
		entity_view.each([&](entt::entity entity, Identity& t)
			{
				ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
				if (selectedEntity == entity)
					flags |= ImGuiTreeNodeFlags_Selected;
				ImGui::TreeNodeEx(Sprintf("%d", (int)entity).c_str(), flags, t.Name.c_str());
				if (ImGui::IsItemClicked(ImGuiMouseButton_Left))
					selectedEntity = entity;
			});
	}
	ImGui::End();

	if (!m_World.Registry.valid(selectedEntity))
		selectedEntity = entt::null;

	if (selectedEntity != entt::null)
	{
		if (ImGui::Begin("Entity"))
		{
			if (Transform* transform = m_World.Registry.try_get<Transform>(selectedEntity))
			{
				Transform& t = *transform;
				if (ImGui::TreeNodeEx("Transform", ImGuiTreeNodeFlags_DefaultOpen))
				{
					static ImGuizmo::OPERATION gizmoOperation(ImGuizmo::ROTATE);
					static ImGuizmo::MODE gizmoMode(ImGuizmo::WORLD);
					if (ImGui::IsKeyPressed(ImGuiKey_W))
						gizmoOperation = ImGuizmo::TRANSLATE;
					if (ImGui::IsKeyPressed(ImGuiKey_E))
						gizmoOperation = ImGuizmo::ROTATE;
					if (ImGui::IsKeyPressed(ImGuiKey_R))
						gizmoOperation = ImGuizmo::SCALE;

					ImGui::DragFloat3("Translation", &t.Position.x, 0.1f);

					// Euler angles are kept alongside the quaternion rather than derived every frame:
					// round-tripping through ToEuler() makes the widget jump near the singularities and
					// loses which of the equivalent angle triplets the user was dragging.
					static entt::entity eulerOwner = entt::null;
					static Vector3 eulerDegrees;
					static Quaternion eulerSource;
					if (eulerOwner != selectedEntity || t.Rotation != eulerSource)
					{
						eulerOwner = selectedEntity;
						eulerSource = t.Rotation;
						eulerDegrees = eulerSource.ToEuler() * Math::RadiansToDegrees;
					}
					if (ImGui::DragFloat3("Rotation", &eulerDegrees.x, 0.5f))
					{
						t.Rotation = Quaternion::CreateFromYawPitchRoll(eulerDegrees * Math::DegreesToRadians);
						eulerSource = t.Rotation;
					}

					ImGui::DragFloat3("Scale", &t.Scale.x, 0.1f);

					if (gizmoOperation != ImGuizmo::SCALE)
					{
						if (ImGui::RadioButton("Local", gizmoMode == ImGuizmo::LOCAL))
							gizmoMode = ImGuizmo::LOCAL;
						ImGui::SameLine();
						if (ImGui::RadioButton("World", gizmoMode == ImGuizmo::WORLD))
							gizmoMode = ImGuizmo::WORLD;
					}

					Matrix worldTransform = Matrix::CreateScale(t.Scale) * Matrix::CreateFromQuaternion(t.Rotation) * Matrix::CreateTranslation(t.Position);
					if (ImGuizmo::Manipulate(&m_Renderer.GetMainView().WorldToView.m[0][0], &m_Renderer.GetMainView().ViewToClipUnjittered.m[0][0], gizmoOperation, gizmoMode, &worldTransform.m[0][0], nullptr, nullptr, nullptr, nullptr))
						worldTransform.Decompose(t.Scale, t.Rotation, t.Position);

					ImGui::TreePop();
				}
			}
			if (Light* light = m_World.Registry.try_get<Light>(selectedEntity))
			{
				Transform& t = m_World.Registry.get<Transform>(selectedEntity);
				DebugRenderer::Get()->AddLight(t, *light, Colors::Yellow);
				if (ImGui::TreeNodeEx("Light", ImGuiTreeNodeFlags_DefaultOpen))
				{
					ImGui::Combo("Type", (int*)&light->Type, gLightTypeStr, (int)LightType::MAX);
					if (light->Type == LightType::Point)
					{
						ImGui::InputFloat("Radius", &light->Range);
					}
					else if (light->Type == LightType::Spot)
					{
						ImGui::InputFloat("Range", &light->Range);
						if (ImGui::SliderAngle("Inner Angle", &light->InnerConeAngle, 0, 179))
							light->OuterConeAngle = Math::Max(light->OuterConeAngle, light->InnerConeAngle);
						if(ImGui::SliderAngle("Outer Angle", &light->OuterConeAngle, 0, 179))
							light->InnerConeAngle = Math::Min(light->OuterConeAngle, light->InnerConeAngle);
					}
					ImGui::ColorEdit3("Color", &light->Colour.x);
					ImGui::InputFloat("Intensity", &light->Intensity);
					ImGui::Checkbox("Cast Shadows", &light->CastShadows);
					ImGui::Checkbox("Volumetric Lighting", &light->VolumetricLighting);
					ImGui::TreePop();
				}
			}
			if (DDGIVolume* ddgi = m_World.Registry.try_get<DDGIVolume>(selectedEntity))
			{
				Transform& t = m_World.Registry.get<Transform>(selectedEntity);
				DebugRenderer::Get()->AddBoundingBox(BoundingBox(Vector3::Zero, ddgi->Extents), t.World, Colors::White);
				if (ImGui::TreeNodeEx("DDGI", ImGuiTreeNodeFlags_DefaultOpen))
				{
					ImGui::SliderFloat3("Extents", &ddgi->Extents.x, -100, 100);
					ImGui::SliderInt3("Probe Count", &ddgi->NumProbes.x, 1, 100);
					ImGui::SliderInt("Max Num Rays", &ddgi->MaxNumRays, 1, 500);
					ImGui::SliderInt("Num Rays", &ddgi->NumRays, 1, 500);
					ImGui::TreePop();
				}
			}
			if (FogVolume* fog = m_World.Registry.try_get<FogVolume>(selectedEntity))
			{
				Transform& t = m_World.Registry.get<Transform>(selectedEntity);
				DebugRenderer::Get()->AddBoundingBox(BoundingBox(Vector3::Zero, fog->Extents), t.World, Colors::White);
				if (ImGui::TreeNodeEx("Fog Volume", ImGuiTreeNodeFlags_DefaultOpen))
				{
					ImGui::SliderFloat3("Extents", &fog->Extents.x, 0, 20);
					ImGui::SliderFloat("Density Base", &fog->DensityBase, 0, 1);
					ImGui::SliderFloat("Density Change", &fog->DensityChange, 0, 1);
					ImGui::ColorEdit3("Color", &fog->Color.x);
					ImGui::TreePop();
				}
			}
			if (Model* pModel = m_World.Registry.try_get<Model>(selectedEntity))
			{
				Transform& t = m_World.Registry.get<Transform>(selectedEntity);
				DebugRenderer::Get()->AddBoundingBox(m_World.Meshes[pModel->MeshIndex].Bounds, t.World, Colors::White);
				if (ImGui::TreeNodeEx("Mesh", ImGuiTreeNodeFlags_DefaultOpen))
				{
					if (pModel->AnimationIndex != -1)
					{
						ImGui::Combo("Animation", &pModel->AnimationIndex, [](void* pUserData, int index)
							{
								const World* pWorld = (World*)pUserData;
								return pWorld->Animations[index].Name.c_str();
							}, &m_World, (int)m_World.Animations.size());
					}
					ImGui::TreePop();
				}
			}
			if (Camera* pCamera = m_World.Registry.try_get<Camera>(selectedEntity))
			{
				ImGui::SliderAngle("Field of View", &pCamera->FOV, 10, 170);
			}
		}
		ImGui::End();
	}
}
