#include "viz.hpp"

#include "gas/gas.hpp"
#include "gas/gas_ui.hpp"
#include "gas/gas_imgui.hpp"
#include "gas/init.hpp"
#include "gas/shader_compiler.hpp"

#include "types.hpp"
#include "sim.hpp"
#include "map_importer.hpp"
#include "mgr.hpp"

#include "viz_shader_common.hpp"

#include <madrona/navmesh.hpp>

#ifndef MADRONA_MW_MODE
#error "Need to link against multi-world madrona lib"
#endif

#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <vector>
#include <random>

#pragma optimize("", off)

namespace NavUtils
{
	using madrona::math::Vector3;
	using madrona::Navmesh;
	int NearestNavTri(const Navmesh& navmesh, Vector3 pos)
	{
        // Keep track of the nearest while looking for containment.
        float closest = FLT_MAX;
        int closestIdx = -1;
        for (int tri = 0; tri < (int)navmesh.numTris; tri++)
        {
            bool contained = true;
            bool gtz = false;
            for (int i = 0; i < 3; i++)
            {
                Vector3 v1 = navmesh.vertices[navmesh.triIndices[tri * 3 + i]];
                Vector3 v2 = navmesh.vertices[navmesh.triIndices[tri * 3 + ((i + 1) % 3)]];
                Vector3 v3 = v2 - v1;
                Vector3 vp = pos - v1;
                Vector3 c = cross(v3, vp);
                if ((c.z > 0.0f) != gtz && i > 0)
                    contained = false;
                gtz = c.z > 0.0f;
                float distsq = v1.distance2(pos);
                if (distsq < closest)
                {
                    float dir = v3.dot(vp);
                    Vector3 perp = vp * (-dir / v3.dot(v3)) + v3;
                    distsq = perp.dot(perp);
                    if (distsq < closest)
                    {
                        closest = fabs(c.z);
                        closestIdx = tri;
                    }
                }
            }
            if (contained)
                return tri;
        }
		return closestIdx;
	}

    Vector3 CenterOfTri(const Navmesh& navmesh, int tri)
    {
        Vector3 center = { 0.0f, 0.0f, 0.0f };
        for (int i = 0; i < 3; i++)
        {
            center += navmesh.vertices[navmesh.triIndices[tri * 3 + i]] / 3.0f;
        }
        return center;
    }

    bool VisitCell(const Navmesh& navmesh, int cell, int targetCell, std::vector<int> &path, std::vector<bool> &visited)
    {
        if (visited[cell])
            return false;
        path.push_back(cell);
        visited[cell] = true;

        if (cell == targetCell)
            return true;
        
        for (int i = 0; i < 3; i++)
        {
            int neighbor = navmesh.triAdjacency[cell * 3 + i];
            if (neighbor < 0 )
                continue;
            if (VisitCell(navmesh, neighbor, targetCell, path, visited))
                return true;
        }
        path.pop_back();
        return false;
    }

    Vector3 BruteForcePathfindToPoint(const Navmesh& navmesh, const Vector3& start, const Vector3& pos)
    {
        // Brute force navigation.
        Vector3 stop = { 0.0f, 0.0f, 0.0f };
        int startTri = NearestNavTri(navmesh, start);
        int posTri = NearestNavTri(navmesh, pos);
        if (startTri < 0 || posTri < 0)
            return stop;
        std::vector<int> path;
        path.reserve(navmesh.numTris);
        std::vector<bool> visited(navmesh.numTris, false);
        if (VisitCell(navmesh, startTri, posTri, path, visited))
        {
            if (path.size() > 1)
                return CenterOfTri(navmesh, path[1]);
            return pos;
        }
        return stop;
    }

    struct Node
    {
        int idx;
        int cameFrom;
        float startDist;
        float score;

        void Clear(int index)
        {
            idx = index;
            cameFrom = -1;
            startDist = FLT_MAX;
            score = FLT_MAX;
        }
    };
    Vector3 AStarPathfindToPoint(const Navmesh& navmesh, const Vector3& start, const Vector3& pos)
    {
		Vector3 stop = { 0.0f, 0.0f, 0.0f };
		int startTri = NearestNavTri(navmesh, start);
		int posTri = NearestNavTri(navmesh, pos);
		if (startTri < 0 || posTri < 0)
			return stop;

        static std::vector<Node> state(navmesh.numTris);
		for (int tri = 0; tri < (int)navmesh.numTris; tri++)
			state[tri].Clear(tri);
        state[startTri].startDist = 0.0f;

        auto sortHeap = [](const int &lhs, const int &rhs)
        {
            return state[lhs].score < state[rhs].score;
        };
        std::set<int, decltype(sortHeap)> heap(sortHeap);
        heap.insert(startTri);

        while (!heap.empty())
        {
            int thisTri = *heap.begin();
            if (thisTri == posTri)
            {
                int goal = thisTri;
                while (goal != startTri && goal != -1)
                {
                    if (state[goal].cameFrom == startTri)
                        return CenterOfTri(navmesh, goal);
                    goal = state[goal].cameFrom;
                }
                return pos;
            }

            heap.erase(heap.begin());
            Vector3 center = CenterOfTri(navmesh, thisTri);
            if (thisTri == startTri)
                center = start;
            for (int i = 0; i < 3; i++)
            {
                int neighbor = navmesh.triAdjacency[thisTri * 3 + i];
                if (neighbor == -1)
                    continue;
                Vector3 neighpos = CenterOfTri(navmesh, neighbor);
                float score = state[thisTri].startDist + center.distance(neighpos);
                if (score < state[neighbor].startDist)
                {
                    state[neighbor].cameFrom = thisTri;
                    state[neighbor].startDist = score;
                    state[neighbor].score = score + neighpos.distance(pos);
                    heap.insert(neighbor);
                }
            }
        }
        return stop;
    }

    Vector3 PathfindToPoint(const Navmesh& navmesh, const Vector3& start, const Vector3& pos)
    {
        //return BruteForcePathfindToPoint(navmesh, start, pos);    
		return AStarPathfindToPoint(navmesh, start, pos);
	}
};

namespace madronaMPEnv {

using namespace gas;

using madrona::math::Mat3x4;

struct MeshMaterial {
  Vector3 color;
};

struct Mesh {
  Buffer buffer;
  u32 vertexOffset;
  u32 indexOffset;
  u32 numTriangles;
  i32 materialIndex;
};

struct Object {
  i32 meshOffset;
  i32 numMeshes;
};

struct AssetGroup {
  Buffer geometryBuffer;
  i32 objectsOffset;
  i32 numObjects;
};

struct FlyCamera {
  Vector3 position;
  Vector3 fwd;
  Vector3 up;
  Vector3 right;

  bool perspective = true;
  float fov = 60.f;
  float orthoHeight = 5.f;
};

static FlyCamera initCam(Vector3 pos, Quat rot)
{
  Vector3 fwd = normalize(rot.rotateVec(math::fwd));
  Vector3 up = normalize(rot.rotateVec(math::up));
  Vector3 right = normalize(cross(fwd, up));

  return FlyCamera {
    .position = pos,
    .fwd = fwd,
    .up = up,
    .right = right,
  };
}

struct VizState {
  UISystem *ui;
  Window *window;

  GPUAPI *gpuAPI;
  GPURuntime *gpu;

  Swapchain swapchain;
  GPUQueue mainQueue;

  ShaderCompilerLib shadercLib;
  ShaderCompiler *shaderc;

  Texture depthAttachment;

  RasterPassInterface onscreenPassInterface;
  RasterPass onscreenPass;

  ParamBlockType globalParamBlockType;
  Buffer globalPassDataBuffer;
  ParamBlock globalParamBlock;

  RasterShader opaqueGeoShader;
  RasterShader agentShader;
  RasterShader goalRegionsShader;
  RasterShader goalRegionsShaderWireframe;
  RasterShader goalRegionsShaderWireframeNoDepth;

  ParamBlockType shotVizParamBlockType;

  RasterShader shotVizShader;

  CommandEncoder enc;

  i32 curWorld;
  i32 curView;
  i32 curControl;

  i32 numWorlds;
  i32 numViews;
  i32 teamSize;

  i32 simTickRate;
  bool doAI[2];

  /*FlyCamera flyCam = {
    .position = {202.869324, 211.050766, 716.584534},
    .fwd = {0.592786, 0.093471, -0.799917},
    .up = {0.790154, 0.124592, 0.600111},
    .right = {0.155756, -0.987796, -0.000000},
  };*/

  /*FlyCamera flyCam = {
	.position = {0.0f, 1500.0f, 1800.0f},
	.fwd = {0.0f,-0.5f, -0.9f}
  };*/
  FlyCamera flyCam = {
	.position = {300.0f, 400.0f, 1200.0f},
	.fwd = {0.0f,-0.2f, -0.9f}
  };

#if 0
    initCam(
    { 1500, 0, 2400.f },
    ( Quat::angleAxis(math::pi / 2.f, math::up) *
      Quat::angleAxis(-0.35f * math::pi, math::right)
    ).normalize()
  );
#endif
  bool linkViewControl = true;
  float cameraMoveSpeed = 2000.f;

  bool showUnmaskedMinimaps = false;

  UserInputEvents simEventsState {};

  std::vector<MeshMaterial> meshMaterials = { { Vector3(1, 1, 1) } };
  std::vector<Mesh> meshes = {};
  std::vector<Object> objects = {};
  std::vector<AssetGroup> objectAssetGroups = {};

  inline VizState() {
      flyCam.fwd = normalize(flyCam.fwd);
      flyCam.right = normalize(cross(flyCam.fwd, Vector3(0.0, 0.0, 1.0)));
      flyCam.up = cross(flyCam.right, flyCam.fwd);
  };
};

struct VizWorld {
  VizState *viz;

  Query<Position, Rotation, Scale, ObjectID> opaqueGeoQuery;
};

static inline float srgbToLinear(float srgb)
{
    if (srgb <= 0.04045f) {
        return srgb / 12.92f;
    }

    return powf((srgb + 0.055f) / 1.055f, 2.4f);
}

static inline Vector4 rgb8ToFloat(uint8_t r, uint8_t g, uint8_t b)
{
    return {
        srgbToLinear((float)r / 255.f),
        srgbToLinear((float)g / 255.f),
        srgbToLinear((float)b / 255.f),
        1.f,
    };
}

static RasterShader loadShader(VizState *viz, const char *path, RasterShaderInit init)
{
  StackAlloc alloc;
  ShaderCompileResult compile_result =
    viz->shaderc->compileShader(alloc, {
      .path = path,
    });

  if (compile_result.diagnostics.size() != 0) {
    fprintf(stderr, "%s", compile_result.diagnostics.data());
  }

  if (!compile_result.success) {
    FATAL("Shader compilation failed!");
  }

  init.byteCode =
      compile_result.getByteCodeForBackend(viz->gpuAPI->backendShaderByteCodeType());

  RasterShader shader = viz->gpu->createRasterShader(init);

  return shader;
}

static void loadObjects(VizState *viz,
                        Span<const imp::SourceObject> objs,
                        Span<const imp::SourceMaterial> materials,
                        Span<const imp::SourceTexture> imported_textures)
{
  i32 new_materials_start_offset = (i32)viz->meshMaterials.size();
  for (const auto &mat : materials) {
    viz->meshMaterials.push_back({
      .color = { mat.color.x, mat.color.y, mat.color.z },
    });
  }

  GPURuntime *gpu = viz->gpu;
  CommandEncoder &enc = viz->enc;

  u32 total_num_bytes;
  {
    u32 cur_num_bytes = 0;
    for (const auto &src_obj : objs) {
      for (const auto &src_mesh : src_obj.meshes) {
        assert(src_mesh.faceCounts == nullptr);

        const u32 num_indices = 3 * src_mesh.numFaces;

        cur_num_bytes = utils::roundUp(cur_num_bytes, (u32)sizeof(OpaqueGeoVertex));
        cur_num_bytes += sizeof(OpaqueGeoVertex) * src_mesh.numVertices;
        cur_num_bytes += sizeof(u32) * src_mesh.numFaces * 3;
      }
    }
    total_num_bytes = cur_num_bytes;
  }

  Buffer staging = gpu->createStagingBuffer(total_num_bytes);
  Buffer mesh_buffer = gpu->createBuffer({
    .numBytes = total_num_bytes,
    .usage = BufferUsage::DrawVertex | BufferUsage::DrawIndex |
        BufferUsage::CopyDst,
  });

  u8 *staging_ptr;
  gpu->prepareStagingBuffers(1, &staging, (void **)&staging_ptr);

  i32 new_objs_start = viz->objects.size();
  u32 cur_buf_offset = 0;
  for (const auto &src_obj : objs) {
    viz->objects.push_back({(i32)viz->meshes.size(), (i32)src_obj.meshes.size()});

    for (const auto &src_mesh : src_obj.meshes) {
      cur_buf_offset = utils::roundUp(cur_buf_offset, (u32)sizeof(OpaqueGeoVertex));
      u32 vertex_offset = cur_buf_offset / sizeof(OpaqueGeoVertex);

      OpaqueGeoVertex *vertex_staging =
          (OpaqueGeoVertex *)(staging_ptr + cur_buf_offset);

      for (i32 i = 0; i < (i32)src_mesh.numVertices; i++) {
        vertex_staging[i] = OpaqueGeoVertex {
          .pos = src_mesh.positions[i],
          .normal = src_mesh.normals[i],
          .uv = src_mesh.uvs[i],
        };
      }

      cur_buf_offset += sizeof(OpaqueGeoVertex) * src_mesh.numVertices;

      u32 index_offset = cur_buf_offset / sizeof(u32);
      u32 *indices_staging = (u32 *)(staging_ptr + cur_buf_offset);

      u32 num_index_bytes = sizeof(u32) * src_mesh.numFaces * 3;
      memcpy(indices_staging, src_mesh.indices, num_index_bytes);
      cur_buf_offset += num_index_bytes;

      viz->meshes.push_back({
        .buffer = mesh_buffer,
        .vertexOffset = vertex_offset,
        .indexOffset = index_offset, 
        .numTriangles = src_mesh.numFaces,
        .materialIndex = src_mesh.materialIDX == -1 ? 0 :
             new_materials_start_offset + (i32)src_mesh.materialIDX,
      });
    }
  }
  assert(cur_buf_offset == total_num_bytes);

  gpu->flushStagingBuffers(1, &staging);

  gpu->waitUntilReady(viz->mainQueue);

  {
    enc.beginEncoding();
    CopyPassEncoder copy_enc = enc.beginCopyPass();

    copy_enc.copyBufferToBuffer(staging, mesh_buffer, 0, 0, total_num_bytes);

    enc.endCopyPass(copy_enc);
    enc.endEncoding();
  }

  gpu->submit(viz->mainQueue, enc);
  gpu->waitUntilWorkFinished(viz->mainQueue);

  gpu->destroyStagingBuffer(staging);

  viz->objectAssetGroups.push_back({
    .geometryBuffer = mesh_buffer,
    .objectsOffset = new_objs_start,
    .numObjects = (i32)objs.size(),
  });
}

static void loadAssets(VizState *viz, const VizConfig &cfg)
{
  auto capsule_path =
      (std::filesystem::path(DATA_DIR) / "capsule.obj").string();
  auto capsule_path_cstr = capsule_path.c_str();
  
  imp::AssetImporter importer;
  std::array<char, 1024> import_err;
  auto capsule_asset = importer.importFromDisk(
      Span(&capsule_path_cstr, 1),
      Span<char>(import_err.data(), import_err.size()));
  
  if (!capsule_asset.has_value()) {
      FATAL("Failed to load capsule: %s", import_err);
  }
  
  auto other_capsule_asset = importer.importFromDisk(
      Span(&capsule_path_cstr, 1),
      Span<char>(import_err.data(), import_err.size()));
  
  auto shot_cylinder_path =
      (std::filesystem::path(DATA_DIR) / "shot_cylinder.obj").string();
  auto shot_cylinder_path_cstr = shot_cylinder_path.c_str();
  auto shot_cylinder_asset_a_miss = importer.importFromDisk(
      Span(&shot_cylinder_path_cstr, 1),
      Span<char>(import_err.data(), import_err.size()));
  
  if (!shot_cylinder_asset_a_miss.has_value()) {
      FATAL("Failed to load shot cylinder: %s", import_err);
  }
  
  auto shot_cylinder_asset_a_hit = importer.importFromDisk(
      Span(&shot_cylinder_path_cstr, 1),
      Span<char>(import_err.data(), import_err.size()));
  
  auto shot_cylinder_asset_b_miss = importer.importFromDisk(
      Span(&shot_cylinder_path_cstr, 1),
      Span<char>(import_err.data(), import_err.size()));
  
  auto shot_cylinder_asset_b_hit = importer.importFromDisk(
      Span(&shot_cylinder_path_cstr, 1),
      Span<char>(import_err.data(), import_err.size()));
  
  auto zone_marker_path =
      (std::filesystem::path(DATA_DIR) / "zone_marker.obj").string();
  auto zone_marker_path_cstr = zone_marker_path.c_str();
  
  auto zone_marker_asset_inactive = importer.importFromDisk(
      Span(&zone_marker_path_cstr, 1),
      Span<char>(import_err.data(), import_err.size()));
  
  auto zone_marker_asset_contested = importer.importFromDisk(
      Span(&zone_marker_path_cstr, 1),
      Span<char>(import_err.data(), import_err.size()));
  
  auto zone_marker_asset_team_a = importer.importFromDisk(
      Span(&zone_marker_path_cstr, 1),
      Span<char>(import_err.data(), import_err.size()));
  
  auto zone_marker_asset_team_b = importer.importFromDisk(
      Span(&zone_marker_path_cstr, 1),
      Span<char>(import_err.data(), import_err.size()));
  
  auto spawn_marker_path =
      (std::filesystem::path(DATA_DIR) / "spawn_marker.obj").string();
  auto spawn_marker_path_cstr = spawn_marker_path.c_str();
  auto spawn_marker_asset_respawn = importer.importFromDisk(
      Span(&spawn_marker_path_cstr, 1),
      Span<char>(import_err.data(), import_err.size()));
  auto spawn_marker_asset_team_a = importer.importFromDisk(
      Span(&spawn_marker_path_cstr, 1),
      Span<char>(import_err.data(), import_err.size()));
  auto spawn_marker_asset_team_b = importer.importFromDisk(
      Span(&spawn_marker_path_cstr, 1),
      Span<char>(import_err.data(), import_err.size()));
  
  auto respawn_region_marker_path =
      (std::filesystem::path(DATA_DIR) / "respawn_region_marker.obj").string();
  auto respawn_region_marker_path_cstr = zone_marker_path.c_str();
  auto respawn_region_marker_asset = importer.importFromDisk(
      Span(&respawn_region_marker_path_cstr, 1),
      Span<char>(import_err.data(), import_err.size()));
  
  auto materials = std::to_array<imp::SourceMaterial>({
      { rgb8ToFloat(230, 230, 230), -1, 0.8f, 1.0f },
      { rgb8ToFloat(20, 20, 230),   -1, 0.8f, 1.0f },
      { rgb8ToFloat(230, 20, 20),   -1, 0.8f, 1.0f },
      { rgb8ToFloat(100, 100, 230), -1, 0.8f, 1.0f },
      { rgb8ToFloat(20, 20, 100),   -1, 0.8f, 1.0f },
      { rgb8ToFloat(230, 100, 100), -1, 0.8f, 1.0f },
      { rgb8ToFloat(100, 20, 20),   -1, 0.8f, 1.0f },
      { rgb8ToFloat(20, 100, 20),   -1, 0.8f, 1.0f },
      { rgb8ToFloat(100, 230, 100),   -1, 0.8f, 1.0f },
  });
  
  auto collision_data_path =
      std::filesystem::path(DATA_DIR) / cfg.mapDataFilename;
  
  AABB world_bounds;
  auto collision_data = importCollisionData(
      collision_data_path.string().c_str(), cfg.mapOffset, cfg.mapRotation,
      &world_bounds);
  
  MapRenderableCollisionData map_render_data =
      convertCollisionDataToRenderMeshes(collision_data);
  
  HeapArray<imp::SourceObject> combined_objects(
      consts::numNonMapAssets + map_render_data.objects.size());
  combined_objects[0] = capsule_asset->objects[0];
  combined_objects[1] = other_capsule_asset->objects[0];
  
  combined_objects[2] = shot_cylinder_asset_a_hit->objects[0];
  combined_objects[3] = shot_cylinder_asset_a_miss->objects[0];
  combined_objects[4] = shot_cylinder_asset_b_hit->objects[0];
  combined_objects[5] = shot_cylinder_asset_b_miss->objects[0];
  combined_objects[6] = zone_marker_asset_inactive->objects[0];
  combined_objects[7] = zone_marker_asset_contested->objects[0];
  combined_objects[8] = zone_marker_asset_team_a->objects[0];
  combined_objects[9] = zone_marker_asset_team_b->objects[0];
  combined_objects[10] = spawn_marker_asset_respawn->objects[0];
  combined_objects[11] = spawn_marker_asset_team_a->objects[0];
  combined_objects[12] = spawn_marker_asset_team_b->objects[0];
  combined_objects[13] = respawn_region_marker_asset->objects[0];
  
  for (CountT i = 0; i < map_render_data.objects.size(); i++) {
      combined_objects[i + consts::numNonMapAssets] = map_render_data.objects[i];
  }
  
  // Capsules
  combined_objects[0].meshes[0].materialIDX = 1;
  combined_objects[1].meshes[0].materialIDX = 2;
  
  // ShotViz?
  combined_objects[2].meshes[0].materialIDX = 3;
  combined_objects[3].meshes[0].materialIDX = 4;
  combined_objects[4].meshes[0].materialIDX = 5;
  combined_objects[5].meshes[0].materialIDX = 6;
  
  // Zone assets
  combined_objects[6].meshes[0].materialIDX = 7;
  combined_objects[7].meshes[0].materialIDX = 8;
  combined_objects[8].meshes[0].materialIDX = 1;
  combined_objects[9].meshes[0].materialIDX = 2;
  
  // Spawn Marker
  combined_objects[10].meshes[0].materialIDX = 8;
  combined_objects[11].meshes[0].materialIDX = 1;
  combined_objects[12].meshes[0].materialIDX = 2;
  
  // Respawn Region Marker
  combined_objects[13].meshes[0].materialIDX = 2;
  
  imp::ImageImporter &tex_importer = importer.imageImporter();
  
  StackAlloc tmp_alloc;
  Span<imp::SourceTexture> imported_textures =
      tex_importer.importImages(tmp_alloc,
  {
  });
  
  loadObjects(viz, combined_objects, materials, imported_textures);
}

namespace VizSystem {

static void vizStep(VizState *viz, Manager &mgr);

static constexpr inline f32 MOUSE_SPEED = 1e-1f;
// FIXME

VizState * init(const VizConfig &cfg)
{
  VizState *viz = new VizState {};

  viz->ui = UISystem::init(UISystem::Config {
    .enableValidation = true,
    .runtimeErrorsAreFatal = true,
  });

  viz->window = viz->ui->createMainWindow(
      "MadronaMPEnv", cfg.windowWidth, cfg.windowHeight);
  
  viz->gpuAPI = viz->ui->gpuAPI();
  GPURuntime *gpu = viz->gpu =
      viz->gpuAPI->createRuntime(0, {viz->window->surface});

  SwapchainProperties swapchain_properties;
  viz->swapchain = gpu->createSwapchain(
      viz->window->surface, &swapchain_properties);

  viz->mainQueue = gpu->getMainQueue();

  viz->shadercLib = InitSystem::loadShaderCompiler();
  auto backend_bytecode_type = viz->gpuAPI->backendShaderByteCodeType();

  viz->shaderc = viz->shadercLib.createCompiler();

  viz->depthAttachment = gpu->createTexture({
    .format = TextureFormat::Depth32_Float,
    .width = (u16)viz->window->pixelWidth,
    .height = (u16)viz->window->pixelHeight,
    .usage = TextureUsage::DepthAttachment,
  });

  viz->onscreenPassInterface = gpu->createRasterPassInterface({
    .uuid = "onscreen_raster_pass"_to_uuid,
    .depthAttachment = { 
      .format = TextureFormat::Depth32_Float,
      .loadMode = AttachmentLoadMode::Clear,
    },
    .colorAttachments = {
      {
        .format = swapchain_properties.format,
        .loadMode = AttachmentLoadMode::Clear,
      },
    },
  });

  viz->onscreenPass = gpu->createRasterPass({
    .interface = viz->onscreenPassInterface,
    .depthAttachment = viz->depthAttachment,
    .colorAttachments = { viz->swapchain.proxyAttachment() },
  });

  ImGuiSystem::init(viz->ui, gpu, viz->mainQueue, viz->shaderc,
      viz->onscreenPassInterface,
      DATA_DIR "imgui_font.ttf", 12.f);

  viz->enc = gpu->createCommandEncoder(viz->mainQueue);

  viz->curWorld = 0;
  viz->curView = 0;
  viz->numWorlds = (i32)cfg.numWorlds;
  viz->numViews = (i32)cfg.numViews;
  viz->teamSize = (i32)cfg.teamSize;
  viz->doAI[0] = cfg.doAITeam1;
  viz->doAI[1] = cfg.doAITeam2;

  viz->simTickRate = 5;

  viz->globalParamBlockType = gpu->createParamBlockType({
    .uuid = "global_pb"_to_uuid,
    .buffers = {
      { 
        .type = BufferBindingType::Uniform,
        .shaderUsage = ShaderStage::Vertex | ShaderStage::Fragment,
      },
    },
  });

  viz->globalPassDataBuffer = gpu->createBuffer({
    .numBytes = sizeof(GlobalPassData),
    .usage = BufferUsage::ShaderUniform | BufferUsage::CopyDst,
  });

  viz->globalParamBlock = gpu->createParamBlock({
    .typeID = viz->globalParamBlockType,
    .buffers = {
      { .buffer = viz->globalPassDataBuffer, .numBytes = sizeof(GlobalPassData) },
    },
  });

  using enum VertexFormat;

  viz->opaqueGeoShader = loadShader(viz, 
    MADRONA_MP_ENV_SRC_DIR "opaque_geo.slang", {
      .byteCode = {},
      .vertexEntry = "vertMain",
      .fragmentEntry = "fragMain",
      .rasterPass = viz->onscreenPassInterface,
      .paramBlockTypes = { viz->globalParamBlockType },
      .numPerDrawBytes = sizeof(OpaqueGeoPerDraw),
      .vertexBuffers = {{ 
        .stride = sizeof(OpaqueGeoVertex), .attributes = {
          { .offset = offsetof(OpaqueGeoVertex, pos), .format = Vec3_F32 },
          { .offset = offsetof(OpaqueGeoVertex, normal),  .format = Vec3_F32 },
          { .offset = offsetof(OpaqueGeoVertex, uv), .format = Vec2_F32 },
        }
      }},
      .rasterConfig = {
        .depthCompare = DepthCompare::GreaterOrEqual,
      },
    });

  viz->agentShader = loadShader(viz, 
    MADRONA_MP_ENV_SRC_DIR "agent.slang", {
      .byteCode = {},
      .vertexEntry = "vertMain",
      .fragmentEntry = "fragMain",
      .rasterPass = viz->onscreenPassInterface,
      .paramBlockTypes = { viz->globalParamBlockType },
      .numPerDrawBytes = sizeof(AgentPerDraw),
      .vertexBuffers = {{ 
        .stride = sizeof(OpaqueGeoVertex), .attributes = {
          { .offset = offsetof(OpaqueGeoVertex, pos), .format = Vec3_F32 },
          { .offset = offsetof(OpaqueGeoVertex, normal),  .format = Vec3_F32 },
          { .offset = offsetof(OpaqueGeoVertex, uv), .format = Vec2_F32 },
        }
      }},
      .rasterConfig = {
        .depthCompare = DepthCompare::Disabled,
        .writeDepth = true,
      },
    });

  viz->goalRegionsShader = loadShader(viz,
    MADRONA_MP_ENV_SRC_DIR "goal_regions.slang", {
      .byteCode = {},
      .vertexEntry = "vertMain",
      .fragmentEntry = "fragMain",
      .rasterPass = viz->onscreenPassInterface,
      .paramBlockTypes = { viz->globalParamBlockType },
      .numPerDrawBytes = sizeof(GoalRegionPerDraw),
      .rasterConfig = {
        .depthCompare = DepthCompare::GreaterOrEqual,
        .writeDepth = true,
        .blending = { BlendingConfig::additiveDefault() },
      },
    });

  viz->goalRegionsShaderWireframe = loadShader(viz, 
    MADRONA_MP_ENV_SRC_DIR "goal_regions.slang", {
      .byteCode = {},
      .vertexEntry = "vertMainWireframe",
      .fragmentEntry = "fragMainWireframe",
      .rasterPass = viz->onscreenPassInterface,
      .paramBlockTypes = { viz->globalParamBlockType },
      .numPerDrawBytes = sizeof(GoalRegionPerDraw),
      .rasterConfig = {
        .depthCompare = DepthCompare::GreaterOrEqual,
        .writeDepth = false,
      },
    });

  viz->goalRegionsShaderWireframeNoDepth = loadShader(viz, 
    MADRONA_MP_ENV_SRC_DIR "goal_regions.slang", {
      .byteCode = {},
      .vertexEntry = "vertMainWireframe",
      .fragmentEntry = "fragMainWireframeNoDepth",
      .rasterPass = viz->onscreenPassInterface,
      .paramBlockTypes = { viz->globalParamBlockType },
      .numPerDrawBytes = sizeof(GoalRegionPerDraw),
      .rasterConfig = {
        .depthCompare = DepthCompare::Disabled,
        .writeDepth = false,
        .cullMode = CullMode::None,
        .blending = { BlendingConfig::additiveDefault() },
      },
    });

  viz->shotVizParamBlockType = gpu->createParamBlockType({
    .uuid = "shot_vz_pb"_to_uuid,
    .buffers = {
      { 
        .type = BufferBindingType::Storage,
        .shaderUsage = ShaderStage::Vertex,
      },
    },
  });

  viz->shotVizShader = loadShader(viz,
    MADRONA_MP_ENV_SRC_DIR "shot_viz.slang", {
      .byteCode = {},
      .vertexEntry = "vertMain",
      .fragmentEntry = "fragMain",
      .rasterPass = viz->onscreenPassInterface,
      .paramBlockTypes = {
        viz->globalParamBlockType,
        viz->shotVizParamBlockType,
      },
      .numPerDrawBytes = 0,
      .rasterConfig = {
        .depthCompare = DepthCompare::Disabled,
        .writeDepth = false,
        .cullMode = CullMode::None,
        .blending = { BlendingConfig::additiveDefault() },
      },
    });


  loadAssets(viz, cfg);

  return viz;
}

void shutdown(VizState *viz)
{
  GPURuntime *gpu = viz->gpu;

  gpu->waitUntilWorkFinished(viz->mainQueue);
  gpu->waitUntilIdle();

  for (AssetGroup &group : viz->objectAssetGroups) {
    gpu->destroyBuffer(group.geometryBuffer);
  }

  gpu->destroyCommandEncoder(viz->enc);

  gpu->destroyRasterShader(viz->shotVizShader);
  gpu->destroyParamBlockType(viz->shotVizParamBlockType);

  gpu->destroyRasterShader(viz->goalRegionsShaderWireframeNoDepth);
  gpu->destroyRasterShader(viz->goalRegionsShaderWireframe);
  gpu->destroyRasterShader(viz->goalRegionsShader);

  gpu->destroyRasterShader(viz->agentShader);

  gpu->destroyRasterShader(viz->opaqueGeoShader);

  ImGuiSystem::shutdown(gpu);

  gpu->destroyParamBlock(viz->globalParamBlock);
  gpu->destroyBuffer(viz->globalPassDataBuffer);
  gpu->destroyParamBlockType(viz->globalParamBlockType);

  gpu->destroyRasterPass(viz->onscreenPass);
  gpu->destroyRasterPassInterface(viz->onscreenPassInterface);

  gpu->destroyTexture(viz->depthAttachment);

  gpu->destroySwapchain(viz->swapchain);

  viz->shadercLib.destroyCompiler(viz->shaderc);
  InitSystem::unloadShaderCompiler(viz->shadercLib);

  viz->gpuAPI->destroyRuntime(gpu);

  viz->ui->destroyMainWindow();
  viz->ui->shutdown();

  delete viz;
}

void initWorld(Context &ctx, VizState *viz)
{
  auto &viz_world = ctx.singleton<VizWorld>();
  viz_world.viz = viz;

  viz_world.opaqueGeoQuery = ctx.query<Position, Rotation, Scale, ObjectID>();
}

static void handleCamera(VizState *viz, float delta_t)
{
  FlyCamera &cam = viz->flyCam;

  Vector3 translate = Vector3::zero();

  const UserInput &input = viz->ui->inputState();

  if (input.isDown(InputID::MouseRight) ||
      input.isDown(InputID::Shift)) {
    viz->ui->enableRawMouseInput(viz->window);

    Vector2 mouse_delta = input.mouseDelta();

    auto around_right = Quat::angleAxis(
      -mouse_delta.y * MOUSE_SPEED * delta_t, cam.right);

    auto around_up = Quat::angleAxis(
      -mouse_delta.x * MOUSE_SPEED * delta_t, math::up);

    auto rotation = (around_up * around_right).normalize();

    cam.up = rotation.rotateVec(cam.up);
    cam.fwd = rotation.rotateVec(cam.fwd);
    cam.right = rotation.rotateVec(cam.right);

    if (input.isDown(InputID::W)) {
      translate += cam.fwd;
    }

    if (input.isDown(InputID::A)) {
      translate -= cam.right;
    }

    if (input.isDown(InputID::S)) {
      translate -= cam.fwd;
    }

    if (input.isDown(InputID::D)) {
      translate += cam.right;
    }
  } else {
    viz->ui->disableRawMouseInput(viz->window);

    if (input.isDown(InputID::W)) {
      translate += cam.up;
    }

    if (input.isDown(InputID::A)) {
      translate -= cam.right;
    }

    if (input.isDown(InputID::S)) {
      translate -= cam.up;
    }

    if (input.isDown(InputID::D)) {
      translate += cam.right;
    }
  }

  cam.position += translate * viz->cameraMoveSpeed * delta_t;

  //printf("\n"
  //       "(%f %f %f)\n"
  //       "(%f %f %f)\n"
  //       "(%f %f %f)\n"
  //       "(%f %f %f)\n\n",
  //  cam.position.x, cam.position.y, cam.position.z,
  //  cam.right.x, cam.right.y, cam.right.z,
  //  cam.up.x, cam.up.y, cam.up.z,
  //  cam.fwd.x, cam.fwd.y, cam.fwd.z);
}

float smoothStep(int step, int range)
{
    float t = float(step) / float(range);
    return (3.0f - 2.0f * t) * t * t;
}

static std::vector<PvPAction> actions;
void planAI(Engine& ctx, VizState* viz, int world, int player)
{
  if (actions.empty())
    return;

  // Get the Agent data.
  Entity agent = ctx.data().agents[player];
  const Magazine &magazine = ctx.get<Magazine>(agent);
  const Navmesh &navmesh = ctx.singleton<LevelData>().navmesh;
  Position agent_pos = ctx.get<Position>(agent);
  Aim agent_aim = ctx.get<Aim>(agent);
  const OpponentsVisibility& enemies = ctx.get<OpponentsVisibility>(agent);
  const HP& hp = ctx.get<HP>(agent);
  const Reward& reward = ctx.get<Reward>(agent);
  const FwdLidar& fwd_lidar = ctx.get<FwdLidar>(agent);
  const RearLidar& rear_lidar = ctx.get<RearLidar>(agent);
  const Zones& zones = ctx.data().zones;
  const ZoneState &zone_mode_state = ctx.singleton<ZoneState>();

  int move_amount = std::rand() % 2;
  int move_angle = std::rand() % 2;
  int r_yaw = std::rand() % 5;
  int r_pitch = 2;// std::rand() % 5;
  int r = magazine.numBullets == 0 ? 1 : 0;
  int stand = std::rand() % 2;

  // If we can see an enemy, fire.
  int f = 0;
  int numAgents = ctx.data().numAgents;
  for (int i = 0; i < numAgents / 2; i++)
  {
    if (enemies.canSee[i])
      f = 1;
  }


  // If there's an active zone, move to it.
  int zoneIdx = zone_mode_state.curZone;
  assert(zoneIdx >= 0 && zoneIdx < zones.numZones);

  // Get a target point in the zone.
  Vector3 center = zones.bboxes[zoneIdx].centroid();

  //Entity agent1 = ctx.data().agents[0];
  //  const GlobalPosObservation& c = ctx.get<GlobalPosObservation>(agent1);
  //  center = Vector3(c.globalX, c.globalY, 0.0f);

  // Pathfind to the target point.
  Vector3 pos = Vector3(agent_pos.x, agent_pos.y, 0.0f);
  center = NavUtils::PathfindToPoint(navmesh, pos, center);
  /*int navTri = NavUtils::NearestNavTri(navmesh, center);
    if (navTri >= 0)
    center = NavUtils::CenterOfTri(navmesh, navTri);*/

  // Turn to face the target, and if we're facing the right way, move forward.
  center.z = 0.0f;
  Vector3 fwd = Vector3(-sin(agent_aim.yaw), cos(agent_aim.yaw));
  Vector3 tgtDir = (center - pos).normalize();
  move_amount = dot(fwd, tgtDir) > 0.6f ? 1 : 0;
  r_yaw = cross(fwd, tgtDir).z < 0.0f ? 0 + move_amount : 4 - move_amount;
  move_amount *= 2;
  move_angle = 0;
  stand = 0;

  // If we're facing a wall, and it's right in our face, push off of it.
  float collisionAng = 0.0f;
  float collisionNorm = 0.0f;
  for (int y = 0; y < consts::fwdLidarHeight; y++)
  {
      for (int x = 0; x < consts::fwdLidarWidth; x++)
      {
          if (fwd_lidar.data[y][x].depth < 16.0f)
          {
              collisionNorm++;
              collisionAng += x;
          }
      }
  }

  // If anything invades our personal space, backpedal from it.
  if (collisionNorm > 0.0f)
  {
      collisionAng /= collisionNorm;
      // The range should be devided into 8 equal segments, but we don't care about the back ones, and we only see half of the extreme side ones.
      // So we look at 16 half-segments...
      move_amount = 1;
      switch ((int)(collisionAng / consts::fwdLidarWidth * 8.0f))
      {
      case 0:
          move_angle = 2;
          break;
	  case 1:
      case 2:
		  move_angle = 3;
          break;
	  case 3:
      case 4:
		  move_angle = 4;
          move_amount = 2;
          break;
	  case 5:
      case 6:
		  move_angle = 5;
          break;
	  case 7:
		  move_angle = 6;
          break;
      }
  }
  
  // Don't try to fire while reloading, and don't try to turn while firing.
  if (r)
      f = 0;
  if (f)
      r_yaw = 2;

  actions[world * viz->numViews + player] = PvPAction{
    .moveAmount = move_amount,
    .moveAngle = move_angle,
    .yawRotate = r_yaw,
    .pitchRotate = r_pitch,
    .fire = f,
    .reload = r,
    .stand = stand,
  };
}

void doAI(VizState* viz, Manager& mgr, int world, int player)
{
	if (actions.empty())
		return;
    mgr.setPvPAction(world, player, actions[world * viz->numViews + player]);
}

void loop(VizState *viz, Manager &mgr)
{
  auto action_tensor = mgr.pvpActionTensor();
  auto self_obs_tensor = mgr.selfObservationTensor();
  auto magazine_tensor = mgr.magazineTensor();
  auto hp_tensor = mgr.hpTensor();

  auto fwd_lidar_tensor = mgr.fwdLidarTensor();
  auto rear_lidar_tensor = mgr.rearLidarTensor();

  auto reward_tensor = mgr.rewardTensor();

  auto match_result_tensor = mgr.matchResultTensor();

  SelfObservation *self_obs_readback = nullptr;
  HP *hp_readback = nullptr;
  Magazine *magazine_readback = nullptr;
  FwdLidar *fwd_lidar_readback = nullptr;
  RearLidar *rear_lidar_readback = nullptr;
  Reward *reward_readback = nullptr;
  MatchResult *match_result_readback = nullptr;

  ExecMode exec_mode = mgr.execMode();
  if (exec_mode == ExecMode::CUDA) {
#ifdef MADRONA_CUDA_SUPPORT
    self_obs_readback = (SelfObservation *)cu::allocReadback(
      sizeof(SelfObservation) * num_views);
    hp_readback = (HP *)cu::allocReadback(
      sizeof(HP) * num_views);
    magazine_readback = (Magazine *)cu::allocReadback(
      sizeof(Magazine) * num_views);
    fwd_lidar_readback = (FwdLidar *)cu::allocReadback(
      sizeof(FwdLidar) * num_views);
    rear_lidar_readback = (RearLidar *)cu::allocReadback(
      sizeof(RearLidar) * num_views);

    reward_readback = (Reward *)cu::allocReadback(
      sizeof(Reward) * num_views);

    match_result_readback = (MatchResult *)cu::allocReadback(
      sizeof(MatchResult));
#endif
  }

  GPURuntime *gpu = viz->gpu;

  UserInputEvents sim_event_state;

  auto last_sim_tick_time = std::chrono::steady_clock::now();
  auto last_frontend_tick_time = std::chrono::steady_clock::now();

  bool running = true;
  while (running) {
    gpu->waitUntilReady(viz->mainQueue);

    auto [swapchain_tex, swapchain_status] =
      gpu->acquireSwapchainImage(viz->swapchain);
    assert(swapchain_status == SwapchainStatus::Valid);

    {
      bool should_exit = viz->ui->processEvents();
      if (should_exit || (viz->window->state & WindowState::ShouldClose) != 
          WindowState::None) {
        running = false;
      }
    }

    viz->simEventsState.merge(viz->ui->inputEvents());

    auto cur_frame_start_time = std::chrono::steady_clock::now();

    float frontend_delta_t;
    {
      std::chrono::duration<float> duration =
          cur_frame_start_time - last_frontend_tick_time;
      frontend_delta_t = duration.count();
    }
    last_frontend_tick_time = cur_frame_start_time;

    auto sim_delta_t = std::chrono::duration<float>(1.f / (float)viz->simTickRate);

    if (cur_frame_start_time - last_sim_tick_time >= sim_delta_t) {
      UserInput &input = viz->ui->inputState();
      UserInputEvents &input_events = viz->simEventsState;
      //world_input_fn(world_input_data, vizCtrl.worldIdx, user_input);

      if (viz->curControl != 0) {
        int32_t x = 0;
        int32_t y = 0;
        int32_t r_yaw = 2;
        int32_t r_pitch = 2;
        int32_t f = 0;
        int32_t r = 0;
        int32_t g = 0;

        int32_t stand;
        {
          PvPAction action_readback;
          PvPAction *src_action = (PvPAction *)action_tensor.devicePtr();
          src_action += viz->curWorld * viz->numViews + viz->curControl;
          memcpy(&action_readback, src_action, sizeof(PvPAction));
          stand = action_readback.stand;
        }

        bool shift_pressed = input.isDown(InputID::Shift);

        if (input.isDown(InputID::R)) {
          r = 1;
        }

        if (input.isDown(InputID::W)) {
          y += 1;
        }
        if (input.isDown(InputID::S)) {
          y -= 1;
        }

        if (input.isDown(InputID::D)) {
          x += 1;
        }
        if (input.isDown(InputID::A)) {
          x -= 1;
        }

        if (input.isDown(InputID::Q)) {
          r_yaw += shift_pressed ? 2 : 1;
        }
        if (input.isDown(InputID::E)) {
          r_yaw -= shift_pressed ? 2 : 1;
        }

        if (input.isDown(InputID::F)) {
          f = 1;
        }

        if (input.isDown(InputID::G)) {
          g = 1;
        }

        if (input.isDown(InputID::C)) {
          stand = (stand + (shift_pressed ? 2 : 1)) % 3;
        }

        if (input.isDown(InputID::Z)) {
          r_pitch += shift_pressed ? 2 : 1;
        }
        if (input.isDown(InputID::X)) {
          r_pitch -= shift_pressed ? 2 : 1;
        }

        int32_t move_amount;
        if (x == 0 && y == 0) {
          move_amount = 0;
        } else if (shift_pressed) {
          move_amount = consts::numMoveAmountBuckets - 1;
        } else {
          move_amount = 1;
        }

        int32_t move_angle;
        if (x == 0 && y == 1) {
          move_angle = 0;
        } else if (x == 1 && y == 1) {
          move_angle = 1;
        } else if (x == 1 && y == 0) {
          move_angle = 2;
        } else if (x == 1 && y == -1) {
          move_angle = 3;
        } else if (x == 0 && y == -1) {
          move_angle = 4;
        } else if (x == -1 && y == -1) {
          move_angle = 5;
        } else if (x == -1 && y == 0) {
          move_angle = 6;
        } else if (x == -1 && y == 1) {
          move_angle = 7;
        } else {
          move_angle = 0;
        }

        mgr.setPvPAction(viz->curWorld, viz->curControl - 1, PvPAction {
          .moveAmount = move_amount,
          .moveAngle = move_angle,
          .yawRotate = r_yaw,
          .pitchRotate = r_pitch,
          .fire = f,
          .reload = r,
          .stand = stand,
        });

        for (int world = 0; world < viz->numWorlds; world++)
        {
            for (int agent = 0; agent < 6; agent++)
            {
                if ((viz->doAI[0] && agent < 3) || (viz->doAI[1] && agent > 2))
                    doAI(viz, mgr, world, agent);
            }
        }

        if (input_events.downEvent(InputID::K)) {
          mgr.setHP(viz->curWorld, viz->curControl - 1, 0);
        }
      }

      if (viz->simEventsState.downEvent(InputID::K1)) {
        mgr.triggerReset(viz->curWorld);
      }

      if (viz->simEventsState.downEvent(InputID::K0)) {
        mgr.setUniformAgentPolicy(AgentPolicy { -1 });
      }

      if (viz->simEventsState.downEvent(InputID::K9)) {
        mgr.setUniformAgentPolicy(AgentPolicy { 0 });
      }

      mgr.step();
      viz->simEventsState.clear();

      //step_fn(step_data);

      last_sim_tick_time = cur_frame_start_time;
    }

    //frame_duration = throttleFPS(cur_frame_start_time);

    if (viz->curControl == 0) {
      handleCamera(viz, frontend_delta_t);
    }
    
    vizStep(viz, mgr);

    gpu->presentSwapchainImage(viz->swapchain);
  }
}

void registerTypes(ECSRegistry &registry)
{
  registry.registerComponent<VizCamera>();
  registry.registerSingleton<VizWorld>();
}

// https://lemire.me/blog/2021/06/03/computing-the-number-of-digits-of-an-integer-even-faster/
static int32_t numDigits(uint32_t x)
{
  static uint64_t table[] = {
      4294967296,  8589934582,  8589934582,  8589934582,  12884901788,
      12884901788, 12884901788, 17179868184, 17179868184, 17179868184,
      21474826480, 21474826480, 21474826480, 21474826480, 25769703776,
      25769703776, 25769703776, 30063771072, 30063771072, 30063771072,
      34349738368, 34349738368, 34349738368, 34349738368, 38554705664,
      38554705664, 38554705664, 41949672960, 41949672960, 41949672960,
      42949672960, 42949672960};

  uint32_t idx = 31 - __builtin_clz(x | 1);
  return (x + table[idx]) >> 32;
}

static void flyCamUI(FlyCamera &cam)
{
  auto side_size = ImGui::CalcTextSize(" Bottom " );
  side_size.y *= 1.4f;
  ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign,
                      ImVec2(0.5f, 0.f));

  if (ImGui::Button("Top", side_size)) {
    cam.position = 10.f * math::up;
    cam.fwd = -math::up;
    cam.up = -math::fwd;
    cam.right = cross(cam.fwd, cam.up);
  }

  ImGui::SameLine();

  if (ImGui::Button("Left", side_size)) {
    cam.position = -10.f * math::right;
    cam.fwd = math::right;
    cam.up = math::up;
    cam.right = cross(cam.fwd, cam.up);
  }

  ImGui::SameLine();

  if (ImGui::Button("Right", side_size)) {
    cam.position = 10.f * math::right;
    cam.fwd = -math::right;
    cam.up = math::up;
    cam.right = cross(cam.fwd, cam.up);
  }

  ImGui::SameLine();

  if (ImGui::Button("Bottom", side_size)) {
    cam.position = -10.f * math::up;
    cam.fwd = math::up;
    cam.up = math::fwd;
    cam.right = cross(cam.fwd, cam.up);
  }

  ImGui::PopStyleVar();

  auto ortho_size = ImGui::CalcTextSize(" Orthographic ");
  ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign,
                      ImVec2(0.5f, 0.f));
  if (ImGui::Selectable("Perspective", cam.perspective, 0,
                        ortho_size)) {
    cam.perspective = true;
  }
  ImGui::SameLine();

  if (ImGui::Selectable("Orthographic", !cam.perspective, 0,
                        ortho_size)) {
    cam.perspective = false;
  }

  ImGui::SameLine();

  ImGui::PopStyleVar();

  ImGui::TextUnformatted("Projection");

  float digit_width = ImGui::CalcTextSize("0").x;
  ImGui::SetNextItemWidth(digit_width * 6);
  if (cam.perspective) {
    ImGui::DragFloat("FOV", &cam.fov, 1.f, 1.f, 179.f, "%.0f");
  } else {
    ImGui::DragFloat("View Size", &cam.orthoHeight,
                     0.5f, 0.f, 100.f, "%0.1f");
  }
}

static Engine & cfgUI(VizState *viz, Manager &mgr)
{
  ImGui::Begin("Controls");

  {
    float worldbox_width = ImGui::CalcTextSize(" ").x * (
      std::max(numDigits(viz->numWorlds) + 2, 7_i32));

    if (viz->numWorlds == 1) {
      ImGui::BeginDisabled();
    }

    ImGui::PushItemWidth(worldbox_width);
    ImGui::DragInt("Current World ID", &viz->curWorld, 1.f, 0,
        viz->numWorlds - 1, "%d", ImGuiSliderFlags_AlwaysClamp);
    ImGui::PopItemWidth();

    if (viz->numWorlds == 1) {
      ImGui::EndDisabled();
    }
  }

  Engine &ctx = mgr.getWorldContext(viz->curWorld);

  ImGui::Checkbox("Control Current View", &viz->linkViewControl);

  {
    StackAlloc str_alloc;
    const char **cam_opts = str_alloc.allocN<const char *>(viz->numViews + 1);
    cam_opts[0] = "Free Camera";

    ImVec2 combo_size = ImGui::CalcTextSize(" Free Camera ");

    for (i32 i = 0; i < viz->numViews; i++) {
      const char *agent_prefix = "Agent ";

      i32 num_bytes = strlen(agent_prefix) + numDigits(i) + 1;
      cam_opts[i + 1] = str_alloc.allocN<char>(num_bytes);
      snprintf((char *)cam_opts[i + 1], num_bytes, "%s%u",
               agent_prefix, (uint32_t)i);
    }

    ImGui::PushItemWidth(combo_size.x * 1.25f);
    if (ImGui::BeginCombo("Current View", cam_opts[viz->curView])) {
      for (i32 i = 0; i < viz->numViews + 1; i++) {
        const bool is_selected = viz->curView == i;
        if (ImGui::Selectable(cam_opts[i], is_selected)) {
          viz->curView = (uint32_t)i;
        }

        if (is_selected) {
          ImGui::SetItemDefaultFocus();
        }
      }

      ImGui::EndCombo();
    }
    ImGui::PopItemWidth();

    if (viz->linkViewControl) {
      viz->curControl = viz->curView;
      ImGui::BeginDisabled();
    }

    ImGui::PushItemWidth(combo_size.x * 1.25f);
    if (ImGui::BeginCombo("Input Control", cam_opts[viz->curControl])) {
      for (CountT i = 0; i < viz->numViews + 1; i++) {
        const bool is_selected = viz->curControl == (i32)i;
        if (ImGui::Selectable(cam_opts[i], is_selected)) {
          viz->curControl = (i32)i;
        }

        if (is_selected) {
          ImGui::SetItemDefaultFocus();
        }
      }

      ImGui::EndCombo();
    }
    ImGui::PopItemWidth();

    if (viz->linkViewControl) {
      ImGui::EndDisabled();
    }
  }

  ImGui::Spacing();
  ImGui::TextUnformatted("Simulation Settings");
  ImGui::Separator();

  ImGui::PushItemWidth(ImGui::CalcTextSize(" ").x * 7);
  ImGui::DragInt("Tick Rate (Hz)", (int *)&viz->simTickRate, 5.f, 0, 1000);
  if (viz->simTickRate < 0) {
    viz->simTickRate = 0;
  }
  ImGui::PopItemWidth();

  ImGui::Spacing();

  if (viz->curView != 0) {
    ImGui::BeginDisabled();
  }

  ImGui::TextUnformatted("Free Camera Config");
  ImGui::Separator();

  flyCamUI(viz->flyCam);

  if (viz->curView != 0) {
    ImGui::EndDisabled();
  }

  ImGui::Spacing();
  ImGui::TextUnformatted("Player Info Settings");
  ImGui::Separator();

  ImGui::Checkbox("Show Unmasked Minimaps", &viz->showUnmaskedMinimaps);

  ImGui::End();

  return ctx;
}

static inline void playerInfoUI(Engine &ctx, VizState *viz)
{
  MatchResult &match_result = ctx.singleton<MatchResult>();

  auto drawDepthArray = [](const LidarData *arr, CountT num_elems, float rescale) {
    float box_width = 10.f;
    float box_height = 10.f;

    for (CountT i = 0; i < num_elems; i++) {
      float d = arr[i].depth;

      ImU32 color;
      if (d == -1.f) {
        color = ImGui::GetColorU32(ImVec4(0.3, 0, 0, 1.f));
      } else {
        float v = fmaxf((d - consts::agentRadius) * rescale, 0.f);
        v = fmaxf(fminf(logf(v + 1.f), 1.f), 0.f);
        color = ImGui::GetColorU32(ImVec4(v, v, v, 1.f));
      }

      ImVec2 p0 = ImGui::GetCursorScreenPos();
      ImVec2 p1 = ImVec2(p0.x + box_width, p0.y + box_height);

      ImGui::GetWindowDrawList()->AddRectFilled(p0, p1, color);
      ImGui::SetCursorScreenPos(ImVec2(p0.x + box_width, p0.y));
    }
  };

  for (int64_t i = 0; i < viz->numViews; i++) {
    auto player_str = std::string("Player ") + std::to_string(i);
    ImGui::Begin(player_str.c_str());
    //float old_size = ImGui::GetFont()->Scale;
    //ImGui::GetFont()->Scale *= 1.5f;
    //ImGui::PushFont(ImGui::GetFont());
    //

    Entity agent = ctx.data().agents[i];

    const Position &cur_pos = ctx.get<Position>(agent);
    const Aim &cur_aim = ctx.get<Aim>(agent);
    const TeamInfo &team = ctx.get<TeamInfo>(agent);

    const HP &hp = ctx.get<HP>(agent);
    const Magazine &mag = ctx.get<Magazine>(agent);
    const Reward &reward = ctx.get<Reward>(agent);

    const FwdLidar &fwd_lidar = ctx.get<FwdLidar>(agent);
    const RearLidar &rear_lidar = ctx.get<RearLidar>(agent);
    const AgentMap &agent_map = ctx.get<AgentMap>(agent);
    const UnmaskedAgentMap &unmasked_agent_map =
        ctx.get<UnmaskedAgentMap>(agent);

    ImGui::Text("Position        (%.1f %.1f %.1f)",
                cur_pos.x, cur_pos.y, cur_pos.z);
    ImGui::Text("Aim             (%.2f, %.2f)",
                cur_aim.yaw, cur_aim.pitch);
    ImGui::Text("HP              %.1f", hp.hp);
    ImGui::Text("Magazine Count  %d", mag.numBullets);
    ImGui::Text("Is Reloading?   %d", mag.isReloading > 0);
    ImGui::Text("Reward          %f", reward.v);
    ImGui::Text("Team            %s",
                team.team == 0 ? "A" : (
                  team.team == 1 ? "B" : "?"));
    ImGui::Spacing();

    ImGui::Text("Fwd Lidar");
    ImVec2 lidar_start = ImGui::GetCursorScreenPos();
    lidar_start.x += 10.f;
    for (CountT y = consts::fwdLidarHeight - 1; y >= 0; y--) {
      ImGui::SetCursorScreenPos(lidar_start);
      drawDepthArray(fwd_lidar.data[y], consts::fwdLidarWidth,
                     1.f / 150.f);
      lidar_start.y += 10.f;
    }

    ImGui::Spacing();
    ImGui::Spacing();
    ImGui::Spacing();
    ImGui::Spacing();

    ImGui::Text("Rear Lidar");
    lidar_start = ImGui::GetCursorScreenPos();
    lidar_start.x += 10.f;
    for (CountT y = consts::rearLidarHeight - 1; y >= 0; y--) {
      ImGui::SetCursorScreenPos(lidar_start);
      drawDepthArray(rear_lidar.data[y], consts::rearLidarWidth,
                     1.f / 150.f);
      lidar_start.y += 10.f;
    }

    ImGui::Spacing();
    ImGui::Spacing();
    ImGui::Spacing();
    ImGui::Spacing();

    ImGui::Text("Minimap");
    {
      const MapItem (*minimap)[AgentMap::res];
      if (viz->showUnmaskedMinimaps) {
        minimap = unmasked_agent_map.data;
      } else {
        minimap = agent_map.data;
      }

      const float rescale = 1.f / 150.f;

      ImVec2 row_start = ImGui::GetCursorScreenPos();
      row_start.x += 10.f;
      for (i32 y = 0; y < AgentMap::res; y++) {
        const float box_width = 10.f;
        const float box_height = 10.f;

        ImGui::SetCursorScreenPos(row_start);
        for (i32 x = 0; x < AgentMap::res; x++) {
          MapItem map_cell = minimap[y][x];

          ImVec4 color_vec = { 0, 0, 0, 1 };
          if (map_cell.numOpponentsPresent > 0) {
            color_vec.x = 1.f;
          }

          if (map_cell.numPastOpponentsPresent > 0) {
            color_vec.y = 1.f;
          }

          if (map_cell.numTeammatesPresent > 0) {
            color_vec.z = 1.f;
          }

          if (map_cell.iAmPresent) {
            color_vec.z = 1.f;
          }

          ImU32 color = ImGui::GetColorU32(color_vec);

          ImVec2 p0 = ImGui::GetCursorScreenPos();
          ImVec2 p1 = ImVec2(p0.x + box_width, p0.y + box_height);

          ImGui::GetWindowDrawList()->AddRectFilled(p0, p1, color);
          ImGui::SetCursorScreenPos(ImVec2(p0.x + box_width, p0.y));
        }

        row_start.y += box_height;
      }
    }

    //ImGui::GetFont()->Scale = old_size;
    //ImGui::PopFont();
    ImGui::End();
  }

  ImGui::Begin("Match Results");
  ImGui::Text("Team A Kills %u", (uint32_t)match_result.teamTotalKills[0]);
  ImGui::Text("Team B Kills %u", (uint32_t)match_result.teamTotalKills[1]);
  ImGui::Text("Team A Obj   %u", (uint32_t)match_result.teamObjectivePoints[0]);
  ImGui::Text("Team B Obj   %u", (uint32_t)match_result.teamObjectivePoints[1]);
  ImGui::End();

  if (viz->doAI[0] || viz->doAI[1]) {
    if (actions.empty())
        actions.resize(viz->numWorlds * viz->numViews);
    for (int agent = 0; agent < viz->numViews; agent++)
    {
        planAI(ctx, viz, ctx.worldID().idx, agent);
    }
  }
}

static Engine & uiLogic(VizState *viz, Manager &mgr)
{
  ImGuiSystem::newFrame(viz->ui, viz->window->systemUIScale, 1.f / 60.f);

  Engine &ctx = cfgUI(viz, mgr);

  playerInfoUI(ctx, viz);

  return ctx;
}

static void setupViewData(Engine &ctx, VizState *viz, GlobalPassData *out)
{
  const FlyCamera &cam = viz->flyCam;

  float aspect_ratio = (f32)viz->window->pixelWidth / viz->window->pixelHeight;

  float fov_scale = 1.f / tanf(math::toRadians(cam.fov * 0.5f));

  float screen_x_scale = fov_scale / aspect_ratio;
  float screen_y_scale = fov_scale;

  float right_dot_pos = -dot(cam.right, cam.position);
  float up_dot_pos = -dot(cam.up, cam.position);
  float fwd_dot_pos = -dot(cam.fwd, cam.position);

  Vector4 w2v_r0 = Vector4::fromVec3W(cam.right, right_dot_pos);
  Vector4 w2v_r1 = Vector4::fromVec3W(cam.up, up_dot_pos);
  Vector4 w2v_r2 = Vector4::fromVec3W(cam.fwd, fwd_dot_pos);

  out->view.w2c.rows[0] = w2v_r0 * screen_x_scale;
  out->view.w2c.rows[1] = w2v_r1 * screen_y_scale;
  out->view.w2c.rows[2] = w2v_r2;

  out->view.fbDims =
    { (u32)viz->window->pixelWidth, (u32)viz->window->pixelHeight };
}

static NonUniformScaleObjectTransform computeNonUniformScaleTxfm(
    Vector3 t, Quat r, Diag3x3 s)
{
  float x2 = r.x * r.x;
  float y2 = r.y * r.y;
  float z2 = r.z * r.z;
  float xz = r.x * r.z;
  float xy = r.x * r.y;
  float yz = r.y * r.z;
  float wx = r.w * r.x;
  float wy = r.w * r.y;
  float wz = r.w * r.z;

  float y2z2 = y2 + z2;
  float x2z2 = x2 + z2;
  float x2y2 = x2 + y2;

  Diag3x3 ds = 2.f * s;
  Diag3x3 i_s = 1.f / s;
  Diag3x3 i_ds = 2.f * i_s;

  NonUniformScaleObjectTransform out;
  out.o2w = {{
    { s.d0 - ds.d0 * y2z2, ds.d1 * (xy - wz), ds.d2 * (xz + wy), t.x },
    { ds.d0 * (xy + wz), s.d1 - ds.d1 * x2z2, ds.d2 * (yz - wx), t.y },
    { ds.d0 * (xz - wy), ds.d1 * (yz + wx), s.d2 - ds.d2 * x2y2, t.z },
  }};

  Vector3 w2o_r0 = 
      { i_s.d0 - i_ds.d0 * y2z2, i_ds.d1 * (xy + wz), ds.d2 * (xz - wy) };
  Vector3 w2o_r1 =
      { i_ds.d0 * (xy - wz), i_s.d1 - i_ds.d1 * x2z2, i_ds.d2 * (yz + wx) };
  Vector3 w2o_r2 =
      { i_ds.d0 * (xz + wy), i_ds.d1 * (yz - wx), i_s.d2 - i_ds.d2 * x2y2 };

  out.w2o = {{
    Vector4::fromVec3W(w2o_r0, -dot(w2o_r0, t)),
    Vector4::fromVec3W(w2o_r1, -dot(w2o_r1, t)),
    Vector4::fromVec3W(w2o_r2, -dot(w2o_r2, t)),
  }};

  return out;
}

static void renderGeo(Engine &ctx, VizState *viz,
                      RasterPassEncoder &raster_enc)
{
  raster_enc.setShader(viz->opaqueGeoShader);
  raster_enc.setParamBlock(0, viz->globalParamBlock);

  ctx.iterateQuery(ctx.singleton<VizWorld>().opaqueGeoQuery,
    [&]
  (Position pos, Rotation rot, Scale scale, ObjectID obj_id)
  {
    Object obj = viz->objects[obj_id.idx];

    for (i32 i = 0; i < obj.numMeshes; i++) {
      Mesh mesh = viz->meshes[i + obj.meshOffset];

      raster_enc.drawData(OpaqueGeoPerDraw {
        .txfm = computeNonUniformScaleTxfm(pos, rot, scale),
        .baseColor = Vector4::fromVec3W(
            viz->meshMaterials[mesh.materialIndex].color, 1.f),
      });

      raster_enc.setVertexBuffer(0, mesh.buffer);
      raster_enc.setIndexBufferU32(mesh.buffer);

      raster_enc.drawIndexed(
          mesh.vertexOffset, mesh.indexOffset, mesh.numTriangles);
    }
  });
}

static void renderGoalRegions(Engine &ctx, VizState *viz,
                              RasterPassEncoder &raster_enc)
{
  const GoalRegionsState &regions_state = ctx.singleton<GoalRegionsState>();

  Vector3 attacker_region_color = { 0, 0, 1 };
  Vector3 defender_region_color = { 1, 0, 0 };

  auto renderGoalRegions = 
    [&]
  (u32 tris_per_region)
  {
    for (int i = 0; i < (int)ctx.data().numGoalRegions; i++) {
      bool region_active = regions_state.regionsActive[i];
      if (!region_active) {
        continue;
      }

      const GoalRegion &region = ctx.data().goalRegions[i];

      for (int j = 0; j < region.numSubRegions; j++) {
        ZOBB zobb = region.subRegions[j];

        Vector3 diag = zobb.pMax - zobb.pMin;
        Vector3 center = 0.5f * (zobb.pMax + zobb.pMin);

        raster_enc.drawData(GoalRegionPerDraw {
          .txfm = computeNonUniformScaleTxfm(
              center, Quat::angleAxis(zobb.rotation, math::up),
              Diag3x3 { diag.x, diag.y, diag.z }),
          .color = Vector4::fromVec3W(
              region.attackerTeam ? attacker_region_color : defender_region_color,
              0.3f),
        });

        raster_enc.draw(0, tris_per_region);
      }
    }
  };

  raster_enc.setParamBlock(0, viz->globalParamBlock);

  raster_enc.setShader(viz->goalRegionsShaderWireframe);
  renderGoalRegions(24);

  raster_enc.setShader(viz->goalRegionsShader);
  renderGoalRegions(12);

  raster_enc.setShader(viz->goalRegionsShaderWireframeNoDepth);
  renderGoalRegions(24);
}

static void renderShotViz(Engine &ctx, VizState *viz,
                          RasterPassEncoder &raster_enc)
{
  GPURuntime *gpu = viz->gpu;
  MappedTmpBuffer line_data_buf;
  int num_lines = 0;

  /*{
      const Navmesh& navmesh = ctx.singleton<LevelData>().navmesh;
      num_lines = navmesh.numTris * 3;

      line_data_buf =
          raster_enc.tmpBuffer(sizeof(ShotVizLineData) * num_lines, 256);

      ShotVizLineData* out_lines = (ShotVizLineData*)line_data_buf.ptr;

      for (int tri = 0; tri < navmesh.numTris; tri++)
      {
          for (int edge = 0; edge < 3; edge++)
          {
              Vector3 a = navmesh.vertices[navmesh.triIndices[tri * 3 + edge]];
              Vector3 b = navmesh.vertices[navmesh.triIndices[tri * 3 + ((edge + 1) % 3)]];

              float alpha = 0.5;

              Vector3 color = { 0, 1, 0 };

              *out_lines++ = {
                .start = a,
                .end = b,
                .color = Vector4::fromVec3W(color, alpha),
              };
          }
      }
  }*/

  {
    const auto& query = ctx.query<ShotVizState, ShotVizRemaining>();

    ctx.iterateQuery(query, [&](ShotVizState&, ShotVizRemaining&)
        {
            num_lines += 1;
        });

    if (num_lines == 0) {
        return;
    }

    line_data_buf =
        raster_enc.tmpBuffer(sizeof(ShotVizLineData) * num_lines, 256);

    ShotVizLineData* out_lines = (ShotVizLineData*)line_data_buf.ptr;

    ctx.iterateQuery(query, [&](ShotVizState& state, ShotVizRemaining& remaining)
        {
            Vector3 a = state.from;
            Vector3 b = state.from + state.dir * state.hitT;

            float alpha = remaining.numStepsRemaining / 30.f;
            alpha *= alpha;

            Vector3 color;
            if (state.team == 0) {
                color = { 0, 0, 1 };
            }
            else {
                color = { 1, 0, 0 };
            }

            if (!state.hit) {
                color *= 0.5f;
                alpha *= 0.25f;
            }

            *out_lines++ = {
                .start = a,
                .end = b,
                .color = Vector4::fromVec3W(color, alpha),
        };
    });
  }

  ParamBlock tmp_geo_block = raster_enc.createTemporaryParamBlock({
    .typeID = viz->shotVizParamBlockType,
    .buffers = {{
      .buffer = line_data_buf.buffer,
      .offset = line_data_buf.offset,
    }},
  });

  raster_enc.setShader(viz->shotVizShader);
  raster_enc.setParamBlock(0, viz->globalParamBlock);
  raster_enc.setParamBlock(1, tmp_geo_block);

  raster_enc.draw(0, num_lines * 2);
}

static void renderAgents(Engine &ctx, VizState *viz,
                         RasterPassEncoder &raster_enc)
{
  GPURuntime *gpu = viz->gpu;

  const auto &query = ctx.query<Position, Rotation, Scale, CombatState,
      Magazine, HP, TeamInfo>();

  raster_enc.setShader(viz->opaqueGeoShader);
  raster_enc.setParamBlock(0, viz->globalParamBlock);

  ctx.iterateQuery(query,
    [&]
  (Vector3 pos, Quat rot, Diag3x3 scale,
   CombatState &combat_state, Magazine &mag, HP &hp, TeamInfo &team_info)
  {
    Object obj = viz->objects[0];

    Vector3 agent_color;

    if (team_info.team == 0) {
      agent_color = { 0, 0, 1 };
    } else {
      agent_color = { 1, 0, 0 };
    }

    if (mag.isReloading) {
      agent_color.y = 1.f;
    }

    float agent_alpha = (float)hp.hp / 100;
    agent_alpha = fmaxf(agent_alpha, 0.1);

    for (i32 i = 0; i < obj.numMeshes; i++) {
      Mesh mesh = viz->meshes[i + obj.meshOffset];

      raster_enc.drawData(AgentPerDraw {
        .txfm = computeNonUniformScaleTxfm(pos, rot, scale),
        .color = Vector4::fromVec3W(i == 0 ? agent_color : Vector3::zero(),
                                    agent_alpha),
      });

      raster_enc.setVertexBuffer(0, mesh.buffer);
      raster_enc.setIndexBufferU32(mesh.buffer);

      raster_enc.drawIndexed(
          mesh.vertexOffset, mesh.indexOffset, mesh.numTriangles);
    }
  });
}

inline void renderSystem(Engine &ctx, VizState *viz)
{
  GPURuntime *gpu = viz->gpu;

  viz->enc.beginEncoding();

  {
    CopyPassEncoder copy_enc = viz->enc.beginCopyPass();
    MappedTmpBuffer global_param_staging =
        copy_enc.tmpBuffer(sizeof(GlobalPassData));

    GlobalPassData *global_param_staging_ptr =
      (GlobalPassData *)global_param_staging.ptr;

    setupViewData(ctx, viz, global_param_staging_ptr);

    copy_enc.copyBufferToBuffer(
        global_param_staging.buffer, viz->globalPassDataBuffer,
        global_param_staging.offset, 0, sizeof(GlobalPassData));

    viz->enc.endCopyPass(copy_enc);
  }

  RasterPassEncoder raster_enc = viz->enc.beginRasterPass(viz->onscreenPass);

  renderGeo(ctx, viz, raster_enc);

  renderAgents(ctx, viz, raster_enc);

  renderGoalRegions(ctx, viz, raster_enc);

  renderShotViz(ctx, viz, raster_enc);

#if 0
  raster_enc.setShader(viz->opaqueGeoShader);
  raster_enc.drawData(Vector3 { 1, 0, 1 });
  raster_enc.draw(0, 1);
#endif

  ImGuiSystem::render(raster_enc);
  viz->enc.endRasterPass(raster_enc);

  viz->enc.endEncoding();
  gpu->submit(viz->mainQueue, viz->enc);
}


void setupGameTasks(VizState *viz, TaskGraphBuilder &builder)
{
}

void vizStep(VizState *viz, Manager &mgr)
{
  Engine &ctx = uiLogic(viz, mgr);

  renderSystem(ctx, viz);
}

}

}