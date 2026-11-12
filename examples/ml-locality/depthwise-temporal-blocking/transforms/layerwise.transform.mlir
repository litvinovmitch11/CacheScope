module attributes {transform.with_named_sequence} {
  transform.named_sequence @__transform_main(
      %root: !transform.any_op {transform.readonly}) {
    // The baseline intentionally preserves the operator-by-operator graph.
    transform.yield
  }
}
