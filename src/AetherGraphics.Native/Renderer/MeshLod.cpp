#include "MeshLod.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

#include "../Core/Log.h"
#include "../Resources/GpuResources.h"
#include "../Scene/SceneStore.h"

#include "meshoptimizer.h"

namespace aether {

namespace {

const uint32_t kMinTriangles = 600;          // a mesh with fewer triangles gets no levels (they would save nothing)
const uint32_t kMinSubMeshTriangles = 32;    // a sub mesh with fewer is copied as it is into every level
// per level: the share of the sub mesh's indices aimed at, and the largest error allowed (share of the mesh's
// bounding radius). The simplifier stops at whichever comes first.
const float kLevelShare[kMaxMeshLods] = {0.5f, 0.25f, 0.1f};
const float kLevelMaxError[kMaxMeshLods] = {0.01f, 0.03f, 0.08f};
const float kMinGain = 0.75f;                // a level with more than 75 % of the triangles of the one before is not kept (nor any after it)
const uint64_t kPageBytes = 8ull << 20;      // the index pages on the GPU
const uint64_t kUploadBytesPerFrame = 8ull << 20;

enum Why { kMade = 0, kSmall, kNoGain, kFailed };

double NowMs() {
	return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

ComPtr<ID3D12Resource> MakeBuffer(ID3D12Device* device, uint64_t bytes, D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES state) {
	D3D12_HEAP_PROPERTIES heap = {};
	heap.Type = type;
	D3D12_RESOURCE_DESC rd = {};
	rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	rd.Width = std::max<uint64_t>(bytes, 256);
	rd.Height = 1;
	rd.DepthOrArraySize = 1;
	rd.MipLevels = 1;
	rd.SampleDesc.Count = 1;
	rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	ComPtr<ID3D12Resource> buffer;
	if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &rd, state, nullptr, IID_PPV_ARGS(&buffer)))) {
		return nullptr;
	}
	return buffer;
}

void RetireResource(ComPtr<ID3D12Resource>& resource, UINT64 fence) {
	if (resource) {
		ComPtr<IUnknown> u;
		resource.As(&u);
		GpuResources::Get().Retire(u, fence);
		resource.Reset();
	}
}

} // namespace

MeshLods& MeshLods::Get() {
	static MeshLods* s = new MeshLods();
	return *s;
}

// ------------------------------------------------------------------------------------------------ SceneBvh's threads

void MeshLods::Submit(uint64_t serial, AeHandle mesh, std::vector<Vec3>&& positions, std::vector<uint32_t>&& indices, std::vector<uint32_t>&& triSubMesh,
	std::vector<AeSubMesh>&& subMeshes, uint32_t indexStride) {
	if (triSubMesh.size() < kMinTriangles) {
		++skippedSmall_;
		return;
	}
	Request r;
	r.serial = serial;
	r.mesh = mesh;
	r.positions = std::move(positions);
	r.indices = std::move(indices);
	r.triSubMesh = std::move(triSubMesh);
	r.subMeshes = std::move(subMeshes);
	r.indexStride = indexStride;
	{
		std::lock_guard lock(mutex_);
		r.generation = generation_;
		if (!worker_.joinable()) {
			stop_ = false;
			worker_ = std::thread([this] { WorkerMain(); });
			SetThreadPriority(worker_.native_handle(), THREAD_PRIORITY_BELOW_NORMAL);
		}
		requests_.push_back(std::move(r));
		++pending_;
	}
	wake_.notify_one();
}

void MeshLods::Forget(AeHandle mesh, uint64_t serial) {
	std::lock_guard lock(mutex_);
	for (auto it = requests_.begin(); it != requests_.end();) {
		if (it->mesh == mesh) {
			it = requests_.erase(it);
			--pending_;
		} else {
			++it;
		}
	}
	forgottenAt_[mesh] = serial;
	pendingForget_.push_back(mesh);
}

void MeshLods::Clear() {
	std::lock_guard lock(mutex_);
	++generation_;
	pending_ -= uint32_t(requests_.size());
	requests_.clear();
	results_.clear();
	pendingForget_.clear();
	forgottenAt_.clear();
}

// ------------------------------------------------------------------------------------------------ worker

void MeshLods::WorkerMain() {
	for (;;) {
		Request r;
		{
			std::unique_lock lock(mutex_);
			wake_.wait(lock, [this] { return stop_ || !requests_.empty(); });
			if (stop_) {
				return;
			}
			r = std::move(requests_.front());
			requests_.pop_front();
		}
		Result result;
		result.generation = r.generation;
		result.serial = r.serial;
		result.mesh = r.mesh;
		const double start = NowMs();
		int why = kFailed;
		const bool made = Build(r, result, why);
		result.ms = NowMs() - start;
		buildMs_.store(buildMs_.load() + result.ms);
		(why == kMade ? made_ : why == kSmall ? skippedSmall_ : why == kNoGain ? skippedNoGain_ : failed_)++;
		if (made) {
			sourceTriangles_ += r.triSubMesh.size();
		}
		std::lock_guard lock(mutex_);
		--pending_;
		if (made && result.generation == generation_) {
			results_.push_back(std::move(result));
		}
	}
}

bool MeshLods::Build(const Request& r, Result& out, int& why) {
	why = kFailed;
	const size_t vertexCount = r.positions.size();
	const size_t triCount = r.triSubMesh.size();
	if (triCount < kMinTriangles) {
		why = kSmall;
		return false;
	}
	if (vertexCount == 0 || r.indices.size() != triCount * 3 || r.subMeshes.empty() || (r.indexStride != 2 && r.indexStride != 4)) {
		return false;
	}
	// the mesh's size (the errors allowed are shares of it)
	Vec3 lo = r.positions[0], hi = r.positions[0];
	for (const Vec3& p : r.positions) {
		if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) {
			return false; // (the simplifier's quadrics would be no numbers)
		}
		lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
		hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
	}
	const float radius = Length(hi - lo) * 0.5f;
	if (!(radius > 0.0f)) {
		return false;
	}
	// the sub meshes' triangles (they come in order: SceneBvh reads them sub mesh by sub mesh)
	const uint32_t subCount = uint32_t(r.subMeshes.size());
	std::vector<uint32_t> first(subCount + 1, 0), count(subCount, 0);
	for (size_t t = 0; t < triCount; ++t) {
		const uint32_t s = r.triSubMesh[t];
		if (s >= subCount || (t > 0 && s < r.triSubMesh[t - 1])) {
			return false;
		}
		if (count[s]++ == 0) {
			first[s] = uint32_t(t);
		}
	}
	for (uint32_t s = 0; s < subCount; ++s) {
		if (count[s] != r.subMeshes[s].indexCount / 3) {
			return false; // (triangles SceneBvh could not read: a level would leave them out where the mesh has them)
		}
	}
	const float* positions = &r.positions[0].x;
	const unsigned int options = meshopt_SimplifyLockBorder | meshopt_SimplifySparse | meshopt_SimplifyErrorAbsolute;
	std::vector<std::vector<uint32_t>> levels; // per level: every sub mesh's indices, one after the other (base vertex added)
	std::vector<uint32_t> ranges;
	std::vector<uint32_t> scratch;
	uint32_t before = uint32_t(triCount);
	out.set.triangles[0] = uint32_t(triCount);
	for (uint32_t level = 0; level < kMaxMeshLods; ++level) {
		std::vector<uint32_t> indices;
		indices.reserve(size_t(triCount * 3 * kLevelShare[level]) + 64);
		std::vector<uint32_t> levelRanges(size_t(subCount) * 2, 0);
		float levelError = 0.0f;
		for (uint32_t s = 0; s < subCount; ++s) {
			const uint32_t* src = r.indices.data() + size_t(first[s]) * 3;
			const size_t srcCount = size_t(count[s]) * 3;
			levelRanges[size_t(s) * 2] = uint32_t(indices.size());
			if (count[s] < kMinSubMeshTriangles) {
				indices.insert(indices.end(), src, src + srcCount);
			} else {
				scratch.resize(srcCount);
				const size_t target = std::max<size_t>(size_t(float(count[s]) * kLevelShare[level]) * 3, 3);
				float error = 0.0f;
				const size_t n = meshopt_simplify(scratch.data(), src, srcCount, positions, vertexCount, sizeof(Vec3), target, kLevelMaxError[level] * radius, options, &error);
				if (n < 3) {
					indices.insert(indices.end(), src, src + srcCount); // (nothing left: the sub mesh stays as it is)
				} else {
					indices.insert(indices.end(), scratch.begin(), scratch.begin() + n);
					levelError = std::max(levelError, error);
				}
			}
			levelRanges[size_t(s) * 2 + 1] = uint32_t(indices.size()) - levelRanges[size_t(s) * 2];
		}
		const uint32_t tris = uint32_t(indices.size() / 3);
		if (float(tris) > float(before) * kMinGain) {
			break; // (no real gain: the coarser levels would not get far either)
		}
		levels.push_back(std::move(indices));
		ranges.insert(ranges.end(), levelRanges.begin(), levelRanges.end());
		out.set.error[level + 1] = levelError;
		out.set.triangles[level + 1] = tris;
		before = tris;
	}
	if (levels.empty()) {
		why = kNoGain;
		return false;
	}
	// into the mesh's index format, relative to each sub mesh's base vertex (as the mesh's own index buffer is), in
	// one buffer; the ranges count from its start
	size_t total = 0;
	for (const auto& l : levels) {
		total += l.size();
	}
	out.indexData.resize(total * r.indexStride);
	size_t at = 0;
	for (size_t level = 0; level < levels.size(); ++level) {
		const std::vector<uint32_t>& l = levels[level];
		for (uint32_t s = 0; s < subCount; ++s) {
			uint32_t& start = ranges[(level * subCount + s) * 2];
			const uint32_t n = ranges[(level * subCount + s) * 2 + 1];
			const uint32_t base = r.subMeshes[s].baseVertex;
			for (uint32_t i = 0; i < n; ++i) {
				const uint32_t v = l[size_t(start) + i];
				if (v < base || (r.indexStride == 2 && v - base > 0xFFFFu)) {
					return false;
				}
				if (r.indexStride == 2) {
					const uint16_t v16 = uint16_t(v - base);
					memcpy(&out.indexData[(at + i) * 2], &v16, 2);
				} else {
					const uint32_t v32 = v - base;
					memcpy(&out.indexData[(at + i) * 4], &v32, 4);
				}
			}
			start = uint32_t(at);
			at += n;
		}
	}
	out.set.levels = uint32_t(levels.size());
	out.set.ranges = std::move(ranges);
	out.set.subMeshes = r.subMeshes;
	out.set.vertexCount = uint32_t(vertexCount);
	out.set.indexStride = r.indexStride;
	why = kMade;
	return true;
}

// ------------------------------------------------------------------------------------------------ render thread

void MeshLods::FreeSet(MeshLodSet& set) {
	if (set.view.BufferLocation && set.page < pages_.size()) {
		Page& p = pages_[set.page];
		p.dead += set.view.SizeInBytes;
		p.sets = p.sets > 0 ? p.sets - 1 : 0;
		uploadedBytes_ -= std::min<uint64_t>(uploadedBytes_, set.view.SizeInBytes);
	}
	set.view = {};
}

void MeshLods::BeginFrame(ID3D12Device* device, ID3D12GraphicsCommandList* list, UINT64 fence) {
	std::vector<Result> results;
	std::vector<AeHandle> forget;
	uint64_t generation;
	{
		std::lock_guard lock(mutex_);
		results.swap(results_);
		forget.swap(pendingForget_);
		generation = generation_;
		// (a mesh forgotten after its geometry was submitted: what was made from that geometry goes)
		results.erase(std::remove_if(results.begin(), results.end(), [this](const Result& r) {
			auto f = forgottenAt_.find(r.mesh);
			return f != forgottenAt_.end() && r.serial <= f->second;
		}), results.end());
	}
	if (generation != appliedGeneration_) {
		// the scene was unloaded: every mesh goes (the pages once the GPU is done with them)
		appliedGeneration_ = generation;
		for (Page& p : pages_) {
			RetireResource(p.buffer, fence);
		}
		pages_.clear();
		sets_.clear();
		waiting_.clear();
		uploadedBytes_ = 0;
	}
	for (AeHandle h : forget) {
		auto it = sets_.find(h);
		if (it != sets_.end()) {
			FreeSet(it->second);
			sets_.erase(it);
		}
		waiting_.erase(std::remove_if(waiting_.begin(), waiting_.end(), [h](const Result& r) { return r.mesh == h; }), waiting_.end());
	}
	for (Result& r : results) {
		if (r.generation == generation) {
			waiting_.push_back(std::move(r));
		}
	}
	// pages that hold nothing any more go (once the GPU is done with them); their slots stay (sets name pages by index)
	for (Page& p : pages_) {
		if (p.buffer && p.sets == 0 && p.used > 0 && p.dead >= p.used) {
			RetireResource(p.buffer, fence);
			p.size = p.used = p.dead = 0;
		}
	}
	uploadsLastFrame_ = 0;
	if (!device || !list || waiting_.empty()) {
		PublishStats();
		return;
	}
	// ---- this frame's uploads: up to the budget, through one staging buffer
	struct Upload { Result* r; uint32_t page; uint64_t offset; uint64_t staging; };
	std::vector<Upload> uploads;
	uint64_t stagingBytes = 0;
	for (Result& r : waiting_) {
		const uint64_t bytes = (uint64_t(r.indexData.size()) + 3) & ~3ull;
		if (!uploads.empty() && stagingBytes + bytes > kUploadBytesPerFrame) {
			break;
		}
		// a page with room (the last one), or a new one (as large as the mesh's indices when they do not fit a page)
		uint32_t page = UINT32_MAX;
		if (!pages_.empty() && pages_.back().buffer && pages_.back().size - pages_.back().used >= bytes) {
			page = uint32_t(pages_.size() - 1);
		} else {
			Page p;
			p.size = std::max(kPageBytes, bytes);
			p.buffer = MakeBuffer(device, p.size, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST);
			if (!p.buffer) {
				AE_LOG_CODE(Warning, "MeshLod", "W_LOD_PAGE", "An index page for the automatic levels of detail could not be made: meshes are drawn in full");
				break;
			}
			p.buffer->SetName(L"Aether.LodIndices");
			p.copyDest = true;
			pages_.push_back(std::move(p));
			page = uint32_t(pages_.size() - 1);
		}
		Page& p = pages_[page];
		uploads.push_back({&r, page, p.used, stagingBytes});
		p.used += bytes;
		stagingBytes += bytes;
	}
	ComPtr<ID3D12Resource> staging = stagingBytes > 0 ? MakeBuffer(device, stagingBytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ) : nullptr;
	void* mapped = nullptr;
	const D3D12_RANGE none = {0, 0};
	if (!staging || FAILED(staging->Map(0, &none, &mapped))) {
		// (the page space taken stays taken: a hole, counted as dead)
		for (const Upload& u : uploads) {
			pages_[u.page].dead += (uint64_t(u.r->indexData.size()) + 3) & ~3ull;
		}
		waiting_.erase(waiting_.begin(), waiting_.begin() + uploads.size());
		PublishStats();
		return;
	}
	std::vector<D3D12_RESOURCE_BARRIER> barriers;
	std::vector<uint32_t> touched;
	for (const Upload& u : uploads) {
		if (std::find(touched.begin(), touched.end(), u.page) == touched.end()) {
			touched.push_back(u.page);
		}
	}
	auto transition = [&](uint32_t page, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
		D3D12_RESOURCE_BARRIER b = {};
		b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		b.Transition.pResource = pages_[page].buffer.Get();
		b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		b.Transition.StateBefore = from;
		b.Transition.StateAfter = to;
		barriers.push_back(b);
	};
	for (uint32_t page : touched) {
		if (!pages_[page].copyDest) {
			transition(page, D3D12_RESOURCE_STATE_INDEX_BUFFER, D3D12_RESOURCE_STATE_COPY_DEST);
		}
	}
	if (!barriers.empty()) {
		list->ResourceBarrier(UINT(barriers.size()), barriers.data());
	}
	for (const Upload& u : uploads) {
		Result& r = *u.r;
		memcpy(static_cast<uint8_t*>(mapped) + u.staging, r.indexData.data(), r.indexData.size());
		list->CopyBufferRegion(pages_[u.page].buffer.Get(), u.offset, staging.Get(), u.staging, r.indexData.size());
		MeshLodSet& set = r.set;
		set.page = u.page;
		set.view.BufferLocation = pages_[u.page].buffer->GetGPUVirtualAddress() + u.offset;
		set.view.SizeInBytes = UINT((uint64_t(r.indexData.size()) + 3) & ~3ull);
		set.view.Format = set.indexStride == 2 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT;
		++pages_[u.page].sets;
		auto existing = sets_.find(r.mesh);
		if (existing != sets_.end()) {
			FreeSet(existing->second); // (read back again: the new one replaces it)
		}
		uploadedBytes_ += set.view.SizeInBytes;
		sets_[r.mesh] = std::move(set);
		++uploadsLastFrame_;
	}
	staging->Unmap(0, nullptr);
	barriers.clear();
	for (uint32_t page : touched) {
		transition(page, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_INDEX_BUFFER);
		pages_[page].copyDest = false;
	}
	list->ResourceBarrier(UINT(barriers.size()), barriers.data());
	RetireResource(staging, fence);
	waiting_.erase(waiting_.begin(), waiting_.begin() + uploads.size());
	PublishStats();
}

const MeshLodSet* MeshLods::Find(AeHandle handle, const SceneMesh& mesh) {
	if (sets_.empty()) {
		return nullptr;
	}
	auto it = sets_.find(handle);
	if (it == sets_.end() || !it->second.view.BufferLocation) {
		return nullptr;
	}
	MeshLodSet& set = it->second;
	ID3D12Resource* ib = mesh.indexBuffer.Get();
	if (ib != set.checkedBuffer) {
		// (once per index buffer the mesh has: the levels were made from what was read back, which must be what is drawn)
		set.checkedBuffer = ib;
		bool same = ib && mesh.desc.vertexCount == set.vertexCount && mesh.desc.indexStride == set.indexStride && mesh.subMeshes.size() == set.subMeshes.size();
		for (size_t s = 0; same && s < set.subMeshes.size(); ++s) {
			const AeSubMesh& a = mesh.subMeshes[s];
			const AeSubMesh& b = set.subMeshes[s];
			same = a.indexStart == b.indexStart && a.indexCount == b.indexCount && a.baseVertex == b.baseVertex;
		}
		set.matches = same;
	}
	return set.matches ? &set : nullptr;
}

void MeshLods::Release(bool leak) {
	{
		std::lock_guard lock(mutex_);
		stop_ = true;
	}
	wake_.notify_all();
	if (worker_.joinable()) {
		worker_.join();
	}
	{
		std::lock_guard lock(mutex_);
		stop_ = false;
		// (what was asked for and not made yet goes: the levels come again with the next scene's geometry)
		pending_ -= uint32_t(requests_.size());
		requests_.clear();
		results_.clear();
	}
	for (Page& p : pages_) {
		if (leak) {
			p.buffer.Detach();
		}
		p.buffer.Reset();
	}
	pages_.clear();
	sets_.clear();
	waiting_.clear();
	uploadedBytes_ = 0;
	PublishStats();
}

void MeshLods::PublishStats() {
	uint64_t levelTris[kMaxMeshLods + 1] = {};
	uint32_t byLevels[kMaxMeshLods + 1] = {};
	for (const auto& [h, set] : sets_) {
		for (uint32_t l = 0; l <= kMaxMeshLods; ++l) {
			levelTris[l] += set.triangles[l];
		}
		++byLevels[std::min(set.levels, kMaxMeshLods)];
	}
	uint64_t pageBytes = 0, deadBytes = 0;
	uint32_t pages = 0;
	for (const Page& p : pages_) {
		if (p.buffer) {
			pageBytes += p.size;
			deadBytes += p.dead;
			++pages;
		}
	}
	size_t waitingBytes = 0;
	for (const Result& r : waiting_) {
		waitingBytes += r.indexData.size();
	}
	json j = {
		{"meshesWithLods", sets_.size()},
		{"meshesByLevelCount", {byLevels[1], byLevels[2], byLevels[3]}},
		{"trianglesPerLevel", {levelTris[0], levelTris[1], levelTris[2], levelTris[3]}},
		{"made", made_.load()}, {"skippedSmall", skippedSmall_.load()}, {"skippedNoGain", skippedNoGain_.load()}, {"failed", failed_.load()},
		{"pending", pending_.load()}, {"waitingUpload", waiting_.size()}, {"uploadsLastFrame", uploadsLastFrame_},
		{"gpuPages", pages}, {"gpuPageBytes", pageBytes}, {"gpuIndexBytes", uploadedBytes_}, {"gpuDeadBytes", deadBytes}, {"cpuWaitingBytes", waitingBytes},
		{"buildMsTotal", buildMs_.load()},
		{"note", "levels at about 50 / 25 / 10 % of the triangles; per instance the coarsest whose error covers at most 2 / lodBias pixels"},
	};
	std::lock_guard lock(statsMutex_);
	stats_ = std::move(j);
}

json MeshLods::Stats() {
	std::lock_guard lock(statsMutex_);
	return stats_.is_null() ? json{{"meshesWithLods", 0}} : stats_;
}

} // namespace aether
