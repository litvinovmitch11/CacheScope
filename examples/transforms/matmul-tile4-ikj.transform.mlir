module attributes {transform.with_named_sequence} {
  transform.named_sequence @__transform_main(
      %root: !transform.any_op {transform.readonly}) {
    %matmul = transform.structured.match ops{["linalg.matmul"]} in %root
      : (!transform.any_op) -> !transform.any_op
    %tiled, %i, %k, %j = transform.structured.tile_using_for %matmul
      tile_sizes [4, 4, 4] interchange = [0, 2, 1]
      : (!transform.any_op) -> (!transform.any_op, !transform.any_op,
                                !transform.any_op, !transform.any_op)
    transform.yield
  }
}
