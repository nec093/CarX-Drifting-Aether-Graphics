// A skinned mesh's pose for the rays, taken on the GPU (src/AetherGraphics.Native/Renderer/Bvh.cpp, RefitPoses): one
// thread per triangle reads its three vertices from Unity's skinned vertex buffer and writes them
//  - into g_Positions, three float3 per triangle: the vertices its bottom level structure is updated from, and
//  - into the mesh's part of the ray triangle pool (BvhTri, shaders/aether_bvh.hlsli): the corner and the two edges.
//    The texture coordinates and the sub mesh there stay as they are.
// The triangles are in the pool's order (g_TriVertices: which vertices each one uses, from the pose read back once).

cbuffer PoseConstants : register(b0) {
	uint g_TriCount;
	uint g_Stride;         // of the skinned vertex buffer
	uint g_PositionOffset; // three 32 bit floats there
	uint g_TriBase;        // the mesh's first triangle in the pool
	uint g_VertexCount;
};

ByteAddressBuffer g_Vertices : register(t0);    // Unity's skinned vertex buffer (stream 0)
ByteAddressBuffer g_TriVertices : register(t1); // three vertex indices per triangle
RWByteAddressBuffer g_Positions : register(u0);
RWByteAddressBuffer g_Tris : register(u1);      // the pool: 64 bytes per triangle (v0 at 0, e1 at 16, e2 at 32)

float3 PoseVertex(uint index) {
	index = min(index, g_VertexCount - 1); // (the buffer has no bounds: a root view)
	return asfloat(g_Vertices.Load3(index * g_Stride + g_PositionOffset));
}

[numthreads(64, 1, 1)]
void CSPose(uint3 id : SV_DispatchThreadID) {
	const uint t = id.x;
	if (t >= g_TriCount) {
		return;
	}
	const uint3 i = g_TriVertices.Load3(t * 12);
	float3 v0 = PoseVertex(i.x), v1 = PoseVertex(i.y), v2 = PoseVertex(i.z);
	// (a vertex that is not a number: the triangle is flattened to a point, there for no ray)
	if (any(!isfinite(v0)) || any(!isfinite(v1)) || any(!isfinite(v2))) {
		v0 = v1 = v2 = 0.0;
	}
	g_Positions.Store3(t * 36, asuint(v0));
	g_Positions.Store3(t * 36 + 12, asuint(v1));
	g_Positions.Store3(t * 36 + 24, asuint(v2));
	const uint at = (g_TriBase + t) * 64;
	g_Tris.Store3(at, asuint(v0));
	g_Tris.Store3(at + 16, asuint(v1 - v0));
	g_Tris.Store3(at + 32, asuint(v2 - v0));
}
