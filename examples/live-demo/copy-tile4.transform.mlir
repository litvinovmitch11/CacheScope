module attributes {transform.with_named_sequence} {
  transform.named_sequence @__transform_main(
      %root: !transform.any_op {transform.readonly}) {
    %copy = transform.structured.match ops{["linalg.copy"]} in %root
      : (!transform.any_op) -> !transform.any_op
    %tiled, %i, %j = transform.structured.tile_using_for %copy
      tile_sizes [4, 4]
      : (!transform.any_op) -> (!transform.any_op, !transform.any_op,
                                !transform.any_op)
    transform.yield
  }
}
