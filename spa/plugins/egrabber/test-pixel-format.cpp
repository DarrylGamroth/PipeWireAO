/* Simple Plugin API */
/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */

#include "pixel_format.hpp"

#include <array>
#include <cstddef>

#include <spa/utils/defs.h>

using egrabber_pipewire::PixelByteOrder;
using egrabber_pipewire::infer_unpacked_byte_order;

int main()
{
	for (const auto format : {"Mono10", "Mono12", "Mono14"}) {
		const std::array<std::byte, 4> little{{
			std::byte{0xa5}, std::byte{0x03},
			std::byte{0x5a}, std::byte{0x02},
		}};
		spa_assert_se(infer_unpacked_byte_order(format, little.data(), little.size()) ==
				PixelByteOrder::little);

		const std::array<std::byte, 4> big{{
			std::byte{0x03}, std::byte{0xa5},
			std::byte{0x02}, std::byte{0x5a},
		}};
		spa_assert_se(infer_unpacked_byte_order(format, big.data(), big.size()) ==
				PixelByteOrder::big);

		const std::array<std::byte, 4> ambiguous{{
			std::byte{0x00}, std::byte{0x01},
			std::byte{0x02}, std::byte{0x03},
		}};
		spa_assert_se(!infer_unpacked_byte_order(format, ambiguous.data(),
				ambiguous.size()));
	}
	return 0;
}
