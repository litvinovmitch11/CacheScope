module attributes {transform.with_named_sequence} {
  transform.named_sequence @__transform_main(
      %root: !transform.any_op {transform.readonly}) {
    %convolutions = transform.structured.match
      ops{["linalg.depthwise_conv_2d_nchw_chw"]} in %root
      : (!transform.any_op) -> !transform.any_op
    %first, %second = transform.split_handle %convolutions
      : (!transform.any_op) -> (!transform.any_op, !transform.any_op)
    %tiled, %height, %width = transform.structured.fuse %second
      tile_sizes [0, 6, 6, 0, 0, 0] {apply_cleanup}
      : (!transform.any_op) -> (!transform.any_op, !transform.any_op,
                                !transform.any_op)
    transform.yield
  }
}
