import torch
import torch.nn.functional as F


class DepthwiseBlock(torch.nn.Module):
    def forward(self, x, weight1, bias1, weight2, bias2):
        hidden = F.conv2d(x, weight1, bias1, groups=2)
        return F.conv2d(torch.relu(hidden), weight2, bias2, groups=2)


def create_model():
    return DepthwiseBlock().eval()
