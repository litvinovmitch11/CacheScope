"""Canonical PyTorch definition and an equivalent temporal-blocked reference."""

from __future__ import annotations

import torch
import torch.nn.functional as functional


BATCH = 1
CHANNELS = 2
HEIGHT = 64
WIDTH = 64
KERNEL = 3
OUTPUT_HEIGHT = HEIGHT - 2 * (KERNEL - 1)
OUTPUT_WIDTH = WIDTH - 2 * (KERNEL - 1)
TILE = 10
SEED = 19


class DepthwiseBlock(torch.nn.Module):
    """Two depthwise 3x3 convolutions with a ReLU between them."""

    def forward(
        self,
        input_tensor: torch.Tensor,
        weight1: torch.Tensor,
        bias1: torch.Tensor,
        weight2: torch.Tensor,
        bias2: torch.Tensor,
    ) -> torch.Tensor:
        hidden = functional.conv2d(
            input_tensor,
            weight1,
            bias1,
            stride=1,
            padding=0,
            groups=CHANNELS,
        )
        activated = torch.relu(hidden)
        return functional.conv2d(
            activated,
            weight2,
            bias2,
            stride=1,
            padding=0,
            groups=CHANNELS,
        )


def temporal_blocked_reference(
    input_tensor: torch.Tensor,
    weight1: torch.Tensor,
    bias1: torch.Tensor,
    weight2: torch.Tensor,
    bias2: torch.Tensor,
    *,
    tile: int = TILE,
) -> torch.Tensor:
    """Compute the same block with one intermediate halo tile at a time.

    The function intentionally preserves the convolution reduction order. It
    is an executable specification of the Transform Dialect schedule, not the
    code used to generate the compiler input graph.
    """

    _, channels, height, width = input_tensor.shape
    if channels != CHANNELS:
        raise ValueError(f"expected {CHANNELS} channels, got {channels}")

    tile_rows: list[torch.Tensor] = []
    output_height = height - 2 * (KERNEL - 1)
    output_width = width - 2 * (KERNEL - 1)
    for output_h0 in range(0, output_height, tile):
        output_h1 = min(output_h0 + tile, output_height)
        tile_columns: list[torch.Tensor] = []
        for output_w0 in range(0, output_width, tile):
            output_w1 = min(output_w0 + tile, output_width)

            input_h0 = output_h0
            input_h1 = output_h1 + 2 * (KERNEL - 1)
            input_w0 = output_w0
            input_w1 = output_w1 + 2 * (KERNEL - 1)
            input_patch = input_tensor[
                :, :, input_h0:input_h1, input_w0:input_w1
            ]
            hidden_patch = functional.conv2d(
                input_patch,
                weight1,
                bias1,
                stride=1,
                padding=0,
                groups=CHANNELS,
            )
            hidden_patch = torch.relu(hidden_patch)
            output_patch = functional.conv2d(
                hidden_patch,
                weight2,
                bias2,
                stride=1,
                padding=0,
                groups=CHANNELS,
            )
            tile_columns.append(output_patch)
        tile_rows.append(torch.cat(tile_columns, dim=3))
    return torch.cat(tile_rows, dim=2)


def make_inputs() -> tuple[torch.Tensor, ...]:
    generator = torch.Generator().manual_seed(SEED)
    input_tensor = torch.randn(
        BATCH, CHANNELS, HEIGHT, WIDTH, generator=generator, dtype=torch.float32
    )
    weight1 = torch.randn(
        CHANNELS, 1, KERNEL, KERNEL, generator=generator, dtype=torch.float32
    )
    bias1 = torch.randn(CHANNELS, generator=generator, dtype=torch.float32)
    weight2 = torch.randn(
        CHANNELS, 1, KERNEL, KERNEL, generator=generator, dtype=torch.float32
    )
    bias2 = torch.randn(CHANNELS, generator=generator, dtype=torch.float32)
    return input_tensor, weight1, bias1, weight2, bias2


def verify_references() -> tuple[tuple[torch.Tensor, ...], torch.Tensor]:
    inputs = make_inputs()
    expected = DepthwiseBlock().eval()(*inputs)
    blocked = temporal_blocked_reference(*inputs)
    torch.testing.assert_close(blocked, expected, rtol=0.0, atol=0.0)
    return inputs, expected


if __name__ == "__main__":
    _, result = verify_references()
    print(
        "depthwise references match exactly: "
        f"shape={tuple(result.shape)}, checksum={result.sum().item():.6f}"
    )
