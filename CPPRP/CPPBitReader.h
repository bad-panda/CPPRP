#pragma once
#include <string>
#include <sstream>
#include <stdint.h>
#include <assert.h>
//#include "ParseException.h"
#include "./data/ReplayFileData.h"
#include "./exceptions/ReplayException.h"
#include <cmath>
#include <memory>


#define QUAT_NUM_BITS (18)
#define MAX_QUAT_VALUE (0.7071067811865475244f)
#define MAX_QUAT_VALUE_INVERSE (1.0f / MAX_QUAT_VALUE)

typedef uint32_t BitReaderType;


namespace CPPRP
{

	static inline float uncompress_quat(uint32_t val)
	{
		const int MaxValue = (1 << QUAT_NUM_BITS) - 1;
		float positiveRangedValue = val / (float)MaxValue;
		float rangedValue = (positiveRangedValue - 0.50f) * 2.0f;
		return rangedValue * MAX_QUAT_VALUE;
	}

	//TODO: memoize?
	static inline const uint32_t msbDeBruijn32(uint32_t v)
	{
		static const int MultiplyDeBruijnBitPosition[32] =
		{
			0, 9, 1, 10, 13, 21, 2, 29, 11, 14, 16, 18, 22, 25, 3, 30,
			8, 12, 20, 28, 15, 17, 24, 7, 19, 27, 23, 6, 26, 5, 4, 31
		};

		v |= v >> 1; // first round down to one less than a power of 2
		v |= v >> 2;
		v |= v >> 4;
		v |= v >> 8;
		v |= v >> 16;

		return static_cast<uint32_t>(MultiplyDeBruijnBitPosition[(const uint32_t)(v * 0x07C4ACDDU) >> 27]);
	}


	//Attempt at writing a fast bitreader for RL replays
	template<typename T>
	class CPPBitReader
	{
	public:
		const T * start;
		const T * data;
		uint32_t t_position;
		uint32_t bit_position;
		const size_t size; //Should be in bits?

		//Let's store this data in here, saves a call to owner.
		//Use during network stream parsing
		const uint16_t engineVersion;
		const uint8_t licenseeVersion;
		const uint8_t netVersion;

		const std::shared_ptr<ReplayFileData> owner;

	private:
		template<typename X>
		const X get_bits(X n)
		{
			#ifndef PARSE_UNSAFE
			if (GetAbsoluteBitPosition() + n > size)
			{
				throw std::runtime_error("Attempted to read beyond buffer");
			}
			#endif

			constexpr uint16_t SIZE_T = sizeof(T) * 8;
			X result = 0;
			X bit_pos = 0;
			T bt = *data;
			if (bit_position > 0)
			{
				if (n > (SIZE_T - bit_position)) //n finishes this byte and needs next byte for sure
				{
					bit_pos += SIZE_T - bit_position;
					n -= SIZE_T - bit_position;
					result |= bt >> bit_position;

					bit_position = 0;
					++t_position;
					bt = *(++data);
				}
				else //n doesn't finish this byte
				{
					result |= (bt >> (bit_position)) & ((1ULL << n) - 1);
					bit_position += n;

					if (bit_position == SIZE_T)
					{
						++data;
						//bt = *(++data);
						++t_position;
						bit_position = 0;
					}

					return result;
				}
			}

			//If we reached this point, we know n > 0
			while (n > (SIZE_T - 1))
			{
				result |= static_cast<X>(bt) << (bit_pos);
				++t_position;
				bt = *(++data); //Valgrind says this has invalid reads, probably at end of replay and there's no bytes left?
				n -= SIZE_T;
				bit_pos += SIZE_T;
			}

			if (n > 0)
			{
				result |= ((bt >> (bit_position)) & ((1ULL << n) - 1)) << bit_pos;
				bit_position += n;

				if (bit_position == SIZE_T)
				{
					++data;
					//bt = *(++data);
					++t_position;
					bit_position = 0;
				}
			}

			return result;
		}

		template<typename X>
		const X get_bits_max(const X maxValue)
		{
			X result = 0;
			
			const uint8_t max_bits = msbDeBruijn32(maxValue);

			result = read<X>(max_bits);

			if((result + (1 << max_bits)) < maxValue)
			{
				result |= (((*data) >> bit_position++) & 1ULL) << max_bits;

				if (bit_position == sizeof(T) * 8)
				{
					++data;
					t_position++;
					bit_position = 0;
				}
			}
			return result;
		}
	public:
		CPPBitReader(const T * data, size_t size, std::shared_ptr<ReplayFileData> owner_);
		CPPBitReader(const T * data, size_t size, std::shared_ptr<ReplayFileData> owner_, 
			const uint32_t engineV, const uint32_t licenseeV, const uint32_t netV);
		CPPBitReader(const CPPBitReader& other);
		CPPBitReader();
		

		template<typename U>
		const U read();
		

		template<typename U>
		const U read(uint16_t customSize);

		/*
		Source from this is from the C# replay parser
		*/
		const float readFixedCompressedFloat(const int32_t maxValue, int32_t numBits);

		template<typename U>
		inline const U readBitsMax(const uint32_t max);

		inline const bool canRead() const noexcept;

		void goback(int32_t num);
		void skip(uint32_t num);

		const size_t GetAbsoluteBytePosition() const noexcept;
		const size_t GetAbsoluteBitPosition() const noexcept;

	};

	template<>
	template<>
	inline const bool CPPBitReader<BitReaderType>::read<bool>()
	{
		return get_bits<uint8_t>(1);
	}

	//Float requires special casting since bit operations aren't allowed
	template<>
	template<>
	inline const float CPPBitReader<BitReaderType>::read<float>()
	{
		assert(sizeof(float) == sizeof(uint32_t));
		uint32_t value = read<uint32_t>();
		return reinterpret_cast<float&>(value);
	}


	template<>
	template<>
	inline const Vector3I CPPBitReader<BitReaderType>::read<Vector3I>()
	{
		//PREFETCH((char*)(this->data));
		const uint32_t maxbits = netVersion >= 7 ? 22 : 20;
		const uint32_t num_bits = readBitsMax<uint32_t>(maxbits);

		const int32_t bias = 1 << (int)(num_bits + 1);
		const int32_t max = (int)num_bits + 2;

		//printf("Test %i\n", num_bits);
		const int32_t dx = read<int32_t>(max);
		const int32_t dy = read<int32_t>(max);
		const int32_t dz = read<int32_t>(max);
		return { (dx - bias), (dy - bias), (dz - bias) };
	}

	template<>
	template<>
	inline const Vector3 CPPBitReader<BitReaderType>::read<Vector3>()
	{
		Vector3I v = read<Vector3I>();
		return { v.x / 100.f, v.y / 100.f, v.z / 100.f };
	}

	template<>
	template<>
	inline const Rotator CPPBitReader<BitReaderType>::read<Rotator>()
	{
		constexpr float conversion = 360.f / 256.f;
		Rotator ret{ 0 };
		if (read<bool>())
		{
			ret.pitch = static_cast<int>(read<int8_t>() * conversion);
		}

		if (read<bool>())
		{
			ret.yaw = static_cast<int>(read<int8_t>()  * conversion);
		}

		if (read<bool>())
		{
			ret.roll = static_cast<int>(read<int8_t>()  * conversion);
		}
		return ret;
	}

	template<>
	template<>
	inline const Quat CPPBitReader<BitReaderType>::read<Quat>()
	{
		uint8_t largest = read<uint8_t>(2);
		const float a = uncompress_quat(read<uint32_t>(QUAT_NUM_BITS));
		const float b = uncompress_quat(read<uint32_t>(QUAT_NUM_BITS));
		const float c = uncompress_quat(read<uint32_t>(QUAT_NUM_BITS));
		const float extra = std::sqrt(1.f - (a*a) - (b*b) - (c * c));

		Quat q = { 0 };
		switch (largest)
		{
		case 0:
			q = { extra, a, b, c };
			break;
		case 1:
			q = { a, extra, b, c };
			break;
		case 2:
			q = { a, b, extra, c };
			break;
		case 3:
		default:
			q = { a, b, c, extra };
			break;
		};
		return q;
	}

	template<>
	template<>
	inline const std::shared_ptr<UniqueId> CPPBitReader<BitReaderType>::read<std::shared_ptr<UniqueId>>()
	{
		std::shared_ptr<UniqueId> uniqueId;
		
		uint8_t platform = read<uint8_t>();
		switch (platform)
		{
		case Platform_Steam:
			uniqueId = std::make_shared<SteamID>();
			std::static_pointer_cast<SteamID>(uniqueId)->steamID = read<uint64_t>(sizeof(uint64_t) * 8);

			break;
		case Platform_Dingo:
			uniqueId = std::make_shared<XBoxID>();
			std::static_pointer_cast<XBoxID>(uniqueId)->xboxID = read<uint64_t>(sizeof(uint64_t) * 8);
			break;
		case Platform_PS4:
			uniqueId = std::make_shared<PS4ID>();
			if (netVersion >= 1)
			{
				std::static_pointer_cast<PS4ID>(uniqueId)->psId = read<uint64_t>(40 * 8);
			}
			else
			{
				std::static_pointer_cast<PS4ID>(uniqueId)->psId = read<uint64_t>(32 * 8);
			}
			break;
		case Platform_Switch:
		{
			std::shared_ptr<SwitchID> switchID = std::make_shared<SwitchID>();
			switchID->a = read<uint64_t>(64);
			switchID->b = read<uint64_t>(64);
			switchID->c = read<uint64_t>(64);
			switchID->d = read<uint64_t>(64);
			uniqueId = switchID;
		}
			break;
		case Platform_PsyNet:
		{
			std::shared_ptr<PsyNetID> psyNetID = std::make_shared<PsyNetID>();
			if (engineVersion >= 868 && licenseeVersion >= 24 && netVersion >= 10)
			{
				psyNetID->a = read<uint64_t>(64);
			}
			else
			{
				psyNetID->a = read<uint64_t>(64);
				psyNetID->b = read<uint64_t>(64);
				psyNetID->c = read<uint64_t>(64);
				psyNetID->d = read<uint64_t>(64);
			}
			uniqueId = psyNetID;
		}
			break;
		case Platform_Unknown:
		{
			uniqueId = std::make_shared<UnkownId>();
			if (licenseeVersion >= 18 && netVersion == 0)
			{
				std::static_pointer_cast<UnkownId>(uniqueId)->unknown = 0;
			}
			else
			{

				std::static_pointer_cast<UnkownId>(uniqueId)->unknown = read<uint32_t>(3 * 8);
			}
		}
			//printf("Unknown platform found!\n");
			break;
		default:
			//printf("Unknown platform %i", id.platform);
			//assert(1 == 2);
			break;
		}
		uniqueId->platform = platform;
		uniqueId->playerNumber= read<uint8_t>();
		return uniqueId;
	}


	template<>
	template<>
	inline const std::string CPPBitReader<BitReaderType>::read<std::string>()
	{
		const int32_t length = read<int32_t>();
		const size_t final_length = static_cast<size_t>(length) * (length > 0 ? 1 : -2);
		if (final_length == 0)
		{
			return "";
		}

		#ifndef PARSE_UNSAFE
		if (final_length > 1024)
		{
			if (engineVersion == 0
				&& licenseeVersion == 0
				&& netVersion == 0)
			{
				throw InvalidVersionException(0,0,0);
			}
			else
			{
				throw std::runtime_error("Got unwanted string length, read value " + std::to_string(length) + ", reading bytes " + std::to_string(final_length) + ". (" + std::to_string(this->bit_position) + ")");
			}
		}
		#endif

		std::string str;
		
		if(bit_position % 8 == 0)
		{
			const char* text = ((char*)data) + (bit_position/8);
			str = std::string(text);
			skip(final_length * 8);
		}
		else
		{
			str.resize(final_length - 1);
			for (size_t i = 0; i < final_length; ++i)
			{
				str[i] = read<uint8_t>();
			}
		}

		return str;
	}

	template<typename T>
	template<typename U>
	inline const U CPPBitReader<T>::readBitsMax(const uint32_t max)
	{
		return get_bits_max<U>(max);
	}

	template<typename T>
	inline CPPBitReader<T>::CPPBitReader(const T * data, size_t sizee, std::shared_ptr<ReplayFileData> owner_) 
	: engineVersion(owner_->header.engineVersion), licenseeVersion(owner_->header.licenseeVersion), 
	netVersion(owner_->header.netVersion), owner(owner_), size(sizee)
	{
		this->start = data;
		this->data = data;
		this->t_position = 0;
		this->bit_position = 0;
	}

	template<typename T>
	inline CPPBitReader<T>::CPPBitReader(const T * data, size_t sizee, std::shared_ptr<ReplayFileData> owner_, 
		const uint32_t engineV, const uint32_t licenseeV, const uint32_t netV) : engineVersion(engineV), 
		licenseeVersion(licenseeV), netVersion(netV), owner(owner_), size(sizee)
	{
		this->start = data;
		this->data = data;
		this->t_position = 0;
		this->bit_position = 0;
	}

	template<typename T>
	inline CPPBitReader<T>::CPPBitReader(const CPPBitReader& other) 
	: engineVersion(other.engineVersion), licenseeVersion(other.licenseeVersion), netVersion(other.netVersion), owner(other.owner), size(other.size)
	{
		this->start = other.start;
		this->data = other.data;
		this->t_position = other.t_position;
		this->bit_position = other.bit_position;
	}

	template<typename T>
	inline CPPBitReader<T>::CPPBitReader() : engineVersion(0), licenseeVersion(0), netVersion(0)
	{
		this->start = NULL;
		this->data = NULL;
		this->size = 0;
		this->t_position = 0;
		this->bit_position = 0;
	}

	template<typename T>
	inline const float CPPBitReader<T>::readFixedCompressedFloat(const int32_t maxValue, const int32_t numBits)
	{
		const int32_t maxBitValue = (1 << (numBits - 1)) - 1;
		const int32_t bias = (1 << (numBits - 1));
		const int32_t serIntMax = (1 << (numBits - 0));

		const int32_t delta = readBitsMax<int32_t>(serIntMax);
		const float unscaledValue = static_cast<float>(delta - bias);

		if (maxValue > maxBitValue)
		{
			// We have to scale down, scale needs to be a float:
			const float invScale = maxValue / (float)maxBitValue;
			return unscaledValue * invScale;
		}
		
		const float scale = maxBitValue / (float)maxValue;
		const float invScale = 1.0f / (float)scale;

		return unscaledValue * invScale;
	}

	template <typename T>
	inline const bool CPPBitReader<T>::canRead() const noexcept
	{
		return GetAbsoluteBitPosition() < size;
	}

	template <typename T>
	void CPPBitReader<T>::goback(int32_t num)
	{
		constexpr uint32_t SIZE_IN_BITS = (sizeof(T) * 8);

		if (static_cast<int32_t>(bit_position) - num < 0)
		{
			num -= bit_position;
			bit_position = SIZE_IN_BITS - (num % SIZE_IN_BITS);
			t_position -= (abs(num)) / SIZE_IN_BITS + 1; 
		}
		else
		{
			bit_position -= num;
		}
		data = start + t_position;
	}

	template<typename T>
	inline void CPPBitReader<T>::skip(uint32_t num)
	{
		constexpr uint32_t SIZE_IN_BITS = (sizeof(T) * 8);
		if (bit_position + num >= SIZE_IN_BITS)
		{
			num -= SIZE_IN_BITS - bit_position;
			bit_position = num % SIZE_IN_BITS;
			t_position += num / SIZE_IN_BITS + 1; // +1 since bit_position + num >= SIZE_IN_BITS
		}
		else
		{
			bit_position += num;
		}
		data = start + t_position;
	}

	template<typename T>
	inline const size_t CPPBitReader<T>::GetAbsoluteBytePosition() const noexcept
	{
		return (t_position * sizeof(T)) + (bit_position / 8);
	}

	template<typename T>
	inline const size_t CPPBitReader<T>::GetAbsoluteBitPosition() const noexcept
	{
		return (t_position * sizeof(T) * 8) + bit_position;
	};

	template<typename T>
	template<typename U>
	inline const U CPPBitReader<T>::read()
	{
		return get_bits<U>(sizeof(U) * 8);
	}

	template<typename T>
	template<typename U>
	inline const U CPPBitReader<T>::read(uint16_t customSize)
	{
		return get_bits<U>(customSize);
	}
}