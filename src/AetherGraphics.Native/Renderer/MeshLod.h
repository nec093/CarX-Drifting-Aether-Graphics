// Automatic levels of detail for static meshes (setting autoLod; docs/architecture/rendering-features.md).
//
// The game's meshes are on the GPU only. The geometry the managed side reads back once per static mesh for ray
// tracing (SceneBvh) is handed here by SceneBvh's worker: positions and the index list per sub mesh. A worker of its
// own (below normal priority) simplifies every sub mesh with meshoptimizer to about 50 %, 25 % and 10 % of its
// triangles (borders locked: no cracks between sub meshes, seams kept), each level with its geometric error in the
// mesh's own units. The simplified indices refer to the mesh's own vertex buffer (relative to the sub mesh's base
// vertex, in the mesh's index format): they are drawn with the mesh's vertex buffers and an index buffer of their
// own, a range of a shared GPU page.
//
// The renderer picks a level per instance from that error: the coarsest level whose error, seen from the camera at
// the nearest point of the instance's bounding sphere, covers no more than 2 / lodBias pixels (SceneRenderer.cpp,
// BuildDrawLists). The shadow cascades may go coarser: down to the level whose error is under half a shadow texel.
//
// Meshes whose vertices change (skinned, marked changing) never get here. A mesh under 600 triangles, or one the
// simplifier cannot bring under 75 % of its triangles, gets no levels.
//
// Threads: Submit from SceneBvh's worker, Forget / Clear on the main thread (SceneBvh's), everything else on the
// render thread, Stats on any thread.
#pragma once
#include "../Core/Common.h"
#include "../Core/Json.h"
#include "../Core/Math.h"
#include "../include/SceneContract.h"

#include <condition_variable>
#include <deque>
#include <thread>
#include <unordered_map>

namespace aether {

struct SceneMesh;

const uint32_t kMaxMeshLods = 3; // levels beyond the mesh itself (LOD0)

struct MeshLodSet {
	uint32_t levels = 0;                    // 1..kMaxMeshLods
	float error[kMaxMeshLods + 1] = {};     // per level: the simplifier's error in the mesh's own units (error[0] = 0)
	uint32_t triangles[kMaxMeshLods + 1] = {}; // per level, all sub meshes ([0]: the mesh's own)
	// per level 1.. and sub mesh: where its indices are in this set's index buffer (start, count; in indices)
	std::vector<uint32_t> ranges;           // ((level - 1) * subMeshCount + subMesh) * 2
	// what it was made from: checked against the mesh that is drawn
	std::vector<AeSubMesh> subMeshes;
	uint32_t vertexCount = 0, indexStride = 0;
	// the index buffer (a range of a page)
	D3D12_INDEX_BUFFER_VIEW view = {};
	uint32_t page = 0;
	// render thread: the drawn mesh's index buffer it was found to match (or not)
	ID3D12Resource* checkedBuffer = nullptr;
	bool matches = false;

	uint32_t Start(uint32_t level, uint32_t subMesh) const { return ranges[(size_t(level - 1) * subMeshes.size() + subMesh) * 2]; }
	uint32_t Count(uint32_t level, uint32_t subMesh) const { return ranges[(size_t(level - 1) * subMeshes.size() + subMesh) * 2 + 1]; }
};

class MeshLods {
public:
	static MeshLods& Get();

	// ---- SceneBvh's worker: a static mesh's geometry. `indices` hold the sub meshes' triangles in order, with the
	// base vertex added (as SceneBvh reads them), `triSubMesh` the sub mesh of each. `serial`: SceneBvh's number for
	// the submission (a mesh forgotten after it was submitted is not made).
	void Submit(uint64_t serial, AeHandle mesh, std::vector<Vec3>&& positions, std::vector<uint32_t>&& indices, std::vector<uint32_t>&& triSubMesh,
		std::vector<AeSubMesh>&& subMeshes, uint32_t indexStride);
	// ---- main thread (under SceneBvh's lock)
	void Forget(AeHandle mesh, uint64_t serial); // the mesh is gone (what was submitted up to `serial`)
	void Clear();                                 // scene unload: every mesh goes

	// ---- render thread
	// Takes in what the worker finished and uploads it (recorded into `list`, before anything is drawn).
	void BeginFrame(ID3D12Device* device, ID3D12GraphicsCommandList* list, UINT64 fence);
	// The levels of `handle` when they are on the GPU and were made from what `mesh` is now. Null: draw the mesh as it is.
	const MeshLodSet* Find(AeHandle handle, const SceneMesh& mesh);
	void Release(bool leak);

	// ---- any thread
	json Stats();

private:
	struct Request {
		uint64_t generation = 0, serial = 0;
		AeHandle mesh = 0;
		std::vector<Vec3> positions;
		std::vector<uint32_t> indices, triSubMesh;
		std::vector<AeSubMesh> subMeshes;
		uint32_t indexStride = 0;
	};
	struct Result {
		uint64_t generation = 0, serial = 0;
		AeHandle mesh = 0;
		MeshLodSet set;
		std::vector<uint8_t> indexData; // every level's indices, in the mesh's index format
		double ms = 0;
	};
	struct Page {
		ComPtr<ID3D12Resource> buffer;
		uint64_t size = 0, used = 0, dead = 0;
		uint32_t sets = 0;
		bool copyDest = true; // its state: COPY_DEST (new), else INDEX_BUFFER
	};
	void WorkerMain();
	// false: no levels for this mesh (`why` says why, for the statistics)
	static bool Build(const Request& r, Result& out, int& why);
	void FreeSet(MeshLodSet& set);
	void PublishStats();

	// main thread / worker <-> render thread
	std::mutex mutex_;
	std::condition_variable wake_;
	std::deque<Request> requests_;
	std::vector<Result> results_;
	std::vector<AeHandle> pendingForget_;
	std::unordered_map<AeHandle, uint64_t> forgottenAt_; // the serial a mesh was forgotten at (since the last Clear)
	std::thread worker_;
	bool stop_ = false;
	uint64_t generation_ = 1;
	std::atomic<uint32_t> pending_{0};
	std::atomic<uint64_t> made_{0}, skippedSmall_{0}, skippedNoGain_{0}, failed_{0};
	std::atomic<uint64_t> sourceTriangles_{0};
	std::atomic<double> buildMs_{0};

	// render thread
	uint64_t appliedGeneration_ = 1;
	std::deque<Result> waiting_; // finished, not uploaded yet (an upload budget per frame)
	std::unordered_map<AeHandle, MeshLodSet> sets_;
	std::vector<Page> pages_;
	uint64_t uploadedBytes_ = 0;
	uint32_t uploadsLastFrame_ = 0;

	std::mutex statsMutex_;
	json stats_;
};

} // namespace aether
