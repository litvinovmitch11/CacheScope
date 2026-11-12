import torch


def create_inputs():
    generator = torch.Generator().manual_seed(19)
    x = torch.randn(1, 2, 64, 64, generator=generator, dtype=torch.float32)
    weight1 = torch.randn(2, 1, 3, 3, generator=generator, dtype=torch.float32)
    bias1 = torch.randn(2, generator=generator, dtype=torch.float32)
    weight2 = torch.randn(2, 1, 3, 3, generator=generator, dtype=torch.float32)
    bias2 = torch.randn(2, generator=generator, dtype=torch.float32)
    return x, weight1, bias1, weight2, bias2
