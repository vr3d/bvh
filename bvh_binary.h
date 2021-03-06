#pragma once

#include <algorithm> // std::min, std::sort
#include <atomic>    // std::atomic
#include <vector>    // std::vector

#include "glm/glm.hpp"

const auto maximum = std::numeric_limits<float>::max();
const auto minimum = std::numeric_limits<float>::lowest();
const auto invalid = std::numeric_limits<uint32_t>::max();

__declspec(align(4))
struct MiniRay
{
	static thread_local glm::vec3 Inverse;
	glm::vec3 Position;
	glm::vec3 Direction;
};

__declspec(align(8))
struct RadianceRay
{
	MiniRay   MiniRay;
	glm::vec3 Barycentric;
	float     Length;
	uint32_t  Face;
};

__forceinline static uint32_t to_uint(float f)
{
	uint32_t ui;
	memcpy(&ui, &f, sizeof(float));
	return ui;
}

__forceinline static float to_float(uint32_t ui)
{
	float f;
	memcpy(&f, &ui, sizeof(uint32_t));
	return f;
}

struct AABB
{
	union
	{
		std::atomic<uint32_t> AtomicMin[3];
		glm::vec3 Min;
	};
	union
	{
		std::atomic<uint32_t> AtomicMax[3];
		glm::vec3 Max;
	};

	__forceinline void update_min(std::atomic<uint32_t>& min_value, const uint32_t value) noexcept
	{
		uint32_t old_value = min_value;
		while (old_value > value && !min_value.compare_exchange_weak(old_value, value))
		{
		}
	}

	__forceinline void update_max(std::atomic<uint32_t>& max_value, const uint32_t value) noexcept
	{
		uint32_t old_value = max_value;
		while (old_value < value && !max_value.compare_exchange_weak(old_value, value))
		{
		}
	}

	AABB() = default;

	void Initialize()
	{
		Min = glm::vec3(maximum);
		Max = glm::vec3(minimum);
	}

	AABB(const AABB& box)
	{
		Min = box.Min;
		Max = box.Max;
	}

	void operator =(const AABB& box)
	{
		Min = box.Min;
		Max = box.Max;
	}

	// expansion by point doesn't need atomic ops
	__forceinline void Expand(const glm::vec3 &p)
	{
		Min = glm::min(p, Min);
		Max = glm::max(p, Max);
	}

	__forceinline void Expand(const AABB &box)
	{
		for (auto i = 0; i < 3; ++i)
		{
			update_min(AtomicMin[i], box.AtomicMin[i]);
			update_max(AtomicMax[i], box.AtomicMax[i]);
		}
	}

	__forceinline glm::vec3 Nomalize(const glm::vec3 &p) const
	{
		return (p - Min) / (Max - Min);
	}

	__forceinline float HalvedSurface() const
	{
		const auto w = glm::max(glm::vec3(0), Max - Min);
		return (w[0] * w[1] + w[1] * w[2] + w[2] * w[0]);
	}

	inline bool Intersect(const MiniRay &ray, float &min, const float length) const
	{
		float x, y, z;
		
		x = ((0 < ray.Inverse.x ? Min.x : Max.x) - ray.Position.x) * ray.Inverse.x;
		y = ((0 < ray.Inverse.y ? Min.y : Max.y) - ray.Position.y) * ray.Inverse.y;
		z = ((0 < ray.Inverse.z ? Min.z : Max.z) - ray.Position.z) * ray.Inverse.z;

		min = glm::max(z, glm::max(x, y));

		x = ((0 < ray.Inverse.x ? Max.x : Min.x) - ray.Position.x) * ray.Inverse.x;
		y = ((0 < ray.Inverse.y ? Max.y : Min.y) - ray.Position.y) * ray.Inverse.y;
		z = ((0 < ray.Inverse.z ? Max.z : Min.z) - ray.Position.z) * ray.Inverse.z;

		const auto max = glm::min(z, glm::min(x, y));

		return (min <= max) && (0.0f < max) && (min < length);
	}
};

__declspec(align(8))
struct Node
{
	AABB Box;

	// Child Indices
	// if lowest bit is 1, leaf
	//                  0, node

	uint32_t L;
	uint32_t R;

	Node() : L(invalid), R(invalid)
	{
		Box.AtomicMax[0] = 0;
		Box.AtomicMax[1] = 0;
		Box.AtomicMax[2] = 0;

		Box.AtomicMin[0] = to_uint(maximum);
		Box.AtomicMin[1] = to_uint(maximum);
		Box.AtomicMin[2] = to_uint(maximum);
	}
};


// Ciprian Apetrei "Fast and Simple Agglomerative LBVH Construction"
// http://diglib.eg.org/handle/10.2312/cgvc.20141206.041-044
struct LBVH
{
	// Thinking Parallel, Part III: Tree Construction on the GPU
	// https://devblogs.nvidia.com/thinking-parallel-part-iii-tree-construction-gpu/

	// Expands a 10-bit integer into 30 bits
	// by inserting 2 zeros after each bit.
	__forceinline uint32_t expandBits(uint32_t v)
	{
		v = (v * 0x00010001u) & 0xFF0000FFu;
		v = (v * 0x00000101u) & 0x0F00F00Fu;
		v = (v * 0x00000011u) & 0xC30C30C3u;
		v = (v * 0x00000005u) & 0x49249249u;
		return v;
	}

	// Calculates a 30-bit Morton code for the
	// given 3D point located within the unit cube [0,1].
	__forceinline uint32_t morton3D(float x, float y, float z)
	{
		x = std::min(std::max(x * 1024.0f, 0.0f), 1023.0f);
		y = std::min(std::max(y * 1024.0f, 0.0f), 1023.0f);
		z = std::min(std::max(z * 1024.0f, 0.0f), 1023.0f);
		uint32_t xx = expandBits((uint32_t)x);
		uint32_t yy = expandBits((uint32_t)y);
		uint32_t zz = expandBits((uint32_t)z);
		return xx * 4 + yy * 2 + zz;
	}

	AABB      Box;  // Scene Bound
	uint32_t  Root; // Root Node ID

	std::vector<Node>      Nodes; // LBVH Nodes (T - 1)
	std::vector<glm::vec3> Ps;    // Positions
	std::vector<uint32_t>  PIDs;  // Position IDs (T x 3)

	void Build();

	// implement your own
	//bool Occlusion(const MiniRay &ray, ...);
	//void Intersect(RadianceRay &ray, ...);
};
