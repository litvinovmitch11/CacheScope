module attributes {cachescope.instrumented, cachescope.origins_assigned} {
  llvm.func @__cachescope_trace_v1(i64, i64, i64, i64, i64) attributes {sym_visibility = "private"}
  llvm.func @matmul_kernel(%arg0: !llvm.ptr, %arg1: !llvm.ptr, %arg2: i64, %arg3: i64, %arg4: i64, %arg5: i64, %arg6: i64, %arg7: !llvm.ptr, %arg8: !llvm.ptr, %arg9: i64, %arg10: i64, %arg11: i64, %arg12: i64, %arg13: i64, %arg14: !llvm.ptr, %arg15: !llvm.ptr, %arg16: i64, %arg17: i64, %arg18: i64, %arg19: i64, %arg20: i64) attributes {sym_visibility = "private"} {
    %0 = llvm.mlir.poison : !llvm.struct<(ptr, ptr, i64, array<2 x i64>, array<2 x i64>)>
    %1 = llvm.insertvalue %arg14, %0[0] : !llvm.struct<(ptr, ptr, i64, array<2 x i64>, array<2 x i64>)>
    %2 = llvm.insertvalue %arg15, %1[1] : !llvm.struct<(ptr, ptr, i64, array<2 x i64>, array<2 x i64>)>
    %3 = llvm.insertvalue %arg16, %2[2] : !llvm.struct<(ptr, ptr, i64, array<2 x i64>, array<2 x i64>)>
    %4 = llvm.insertvalue %arg17, %3[3, 0] : !llvm.struct<(ptr, ptr, i64, array<2 x i64>, array<2 x i64>)>
    %5 = llvm.insertvalue %arg19, %4[4, 0] : !llvm.struct<(ptr, ptr, i64, array<2 x i64>, array<2 x i64>)>
    %6 = llvm.insertvalue %arg18, %5[3, 1] : !llvm.struct<(ptr, ptr, i64, array<2 x i64>, array<2 x i64>)>
    %7 = llvm.insertvalue %arg20, %6[4, 1] : !llvm.struct<(ptr, ptr, i64, array<2 x i64>, array<2 x i64>)>
    %8 = llvm.mlir.poison : !llvm.struct<(ptr, ptr, i64, array<2 x i64>, array<2 x i64>)>
    %9 = llvm.insertvalue %arg7, %8[0] : !llvm.struct<(ptr, ptr, i64, array<2 x i64>, array<2 x i64>)>
    %10 = llvm.insertvalue %arg8, %9[1] : !llvm.struct<(ptr, ptr, i64, array<2 x i64>, array<2 x i64>)>
    %11 = llvm.insertvalue %arg9, %10[2] : !llvm.struct<(ptr, ptr, i64, array<2 x i64>, array<2 x i64>)>
    %12 = llvm.insertvalue %arg10, %11[3, 0] : !llvm.struct<(ptr, ptr, i64, array<2 x i64>, array<2 x i64>)>
    %13 = llvm.insertvalue %arg12, %12[4, 0] : !llvm.struct<(ptr, ptr, i64, array<2 x i64>, array<2 x i64>)>
    %14 = llvm.insertvalue %arg11, %13[3, 1] : !llvm.struct<(ptr, ptr, i64, array<2 x i64>, array<2 x i64>)>
    %15 = llvm.insertvalue %arg13, %14[4, 1] : !llvm.struct<(ptr, ptr, i64, array<2 x i64>, array<2 x i64>)>
    %16 = llvm.mlir.poison : !llvm.struct<(ptr, ptr, i64, array<2 x i64>, array<2 x i64>)>
    %17 = llvm.insertvalue %arg0, %16[0] : !llvm.struct<(ptr, ptr, i64, array<2 x i64>, array<2 x i64>)>
    %18 = llvm.insertvalue %arg1, %17[1] : !llvm.struct<(ptr, ptr, i64, array<2 x i64>, array<2 x i64>)>
    %19 = llvm.insertvalue %arg2, %18[2] : !llvm.struct<(ptr, ptr, i64, array<2 x i64>, array<2 x i64>)>
    %20 = llvm.insertvalue %arg3, %19[3, 0] : !llvm.struct<(ptr, ptr, i64, array<2 x i64>, array<2 x i64>)>
    %21 = llvm.insertvalue %arg5, %20[4, 0] : !llvm.struct<(ptr, ptr, i64, array<2 x i64>, array<2 x i64>)>
    %22 = llvm.insertvalue %arg4, %21[3, 1] : !llvm.struct<(ptr, ptr, i64, array<2 x i64>, array<2 x i64>)>
    %23 = llvm.insertvalue %arg6, %22[4, 1] : !llvm.struct<(ptr, ptr, i64, array<2 x i64>, array<2 x i64>)>
    %24 = llvm.mlir.constant(3 : i64) : i64
    %25 = llvm.mlir.constant(2 : i64) : i64
    %26 = llvm.mlir.constant(1 : i64) : i64
    %27 = llvm.mlir.constant(4 : i64) : i64
    %28 = llvm.mlir.constant(0 : i64) : i64
    %29 = llvm.mlir.constant(1 : index) : i64
    %30 = llvm.mlir.constant(0 : index) : i64
    %31 = llvm.mlir.constant(16 : index) : i64
    %32 = llvm.mlir.constant(4 : index) : i64
    llvm.br ^bb1(%30 : i64)
  ^bb1(%33: i64):  // 2 preds: ^bb0, ^bb17
    %34 = llvm.icmp "slt" %33, %31 : i64
    llvm.cond_br %34, ^bb2, ^bb18
  ^bb2:  // pred: ^bb1
    llvm.br ^bb3(%30 : i64)
  ^bb3(%35: i64):  // 2 preds: ^bb2, ^bb16
    %36 = llvm.icmp "slt" %35, %31 : i64
    llvm.cond_br %36, ^bb4, ^bb17
  ^bb4:  // pred: ^bb3
    llvm.br ^bb5(%30 : i64)
  ^bb5(%37: i64):  // 2 preds: ^bb4, ^bb15
    %38 = llvm.icmp "slt" %37, %31 : i64
    llvm.cond_br %38, ^bb6, ^bb16
  ^bb6:  // pred: ^bb5
    %39 = llvm.extractvalue %23[0] : !llvm.struct<(ptr, ptr, i64, array<2 x i64>, array<2 x i64>)>
    %40 = llvm.extractvalue %23[1] : !llvm.struct<(ptr, ptr, i64, array<2 x i64>, array<2 x i64>)>
    %41 = llvm.mlir.poison : !llvm.struct<(ptr, ptr, i64)>
    %42 = llvm.insertvalue %39, %41[0] : !llvm.struct<(ptr, ptr, i64)>
    %43 = llvm.insertvalue %40, %42[1] : !llvm.struct<(ptr, ptr, i64)>
    %44 = llvm.mlir.constant(0 : index) : i64
    %45 = llvm.insertvalue %44, %43[2] : !llvm.struct<(ptr, ptr, i64)>
    %46 = llvm.extractvalue %23[2] : !llvm.struct<(ptr, ptr, i64, array<2 x i64>, array<2 x i64>)>
    %47 = llvm.extractvalue %23[3, 0] : !llvm.struct<(ptr, ptr, i64, array<2 x i64>, array<2 x i64>)>
    %48 = llvm.extractvalue %23[3, 1] : !llvm.struct<(ptr, ptr, i64, array<2 x i64>, array<2 x i64>)>
    %49 = llvm.extractvalue %23[4, 0] : !llvm.struct<(ptr, ptr, i64, array<2 x i64>, array<2 x i64>)>
    %50 = llvm.extractvalue %23[4, 1] : !llvm.struct<(ptr, ptr, i64, array<2 x i64>, array<2 x i64>)>
    %51 = llvm.mlir.constant(16 : index) : i64
    %52 = llvm.mul %33, %51 overflow<nsw> : i64
    %53 = llvm.add %52, %35 : i64
    %54 = llvm.mlir.poison : !llvm.struct<(ptr, ptr, i64, array<2 x i64>, array<2 x i64>)>
    %55 = llvm.extractvalue %45[0] : !llvm.struct<(ptr, ptr, i64)>
    %56 = llvm.extractvalue %45[1] : !llvm.struct<(ptr, ptr, i64)>
    %57 = llvm.insertvalue %55, %54[0] : !llvm.struct<(ptr, ptr, i64, array<2 x i64>, array<2 x i64>)>
    %58 = llvm.insertvalue %56, %57[1] : !llvm.struct<(ptr, ptr, i64, array<2 x i64>, array<2 x i64>)>
    %59 = llvm.insertvalue %53, %58[2] : !llvm.struct<(ptr, ptr, i64, array<2 x i64>, array<2 x i64>)>
    %60 = llvm.mlir.constant(4 : index) : i64
    %61 = llvm.insertvalue %60, %59[3, 0] : !llvm.struct<(ptr, ptr, i64, array<2 x i64>, array<2 x i64>)>
    %62 = llvm.mlir.constant(16 : index) : i64
    %63 = llvm.insertvalue %62, %61[4, 0] : !llvm.struct<(ptr, ptr, i64, array<2 x i64>, array<2 x i64>)>
    %64 = llvm.mlir.constant(4 : index) : i64
    %65 = llvm.insertvalue %64, %63[3, 1] : !llvm.struct<(ptr, ptr, i64, array<2 x i64>, array<2 x i64>)>
    %66 = llvm.mlir.constant(1 : index) : i64
    %67 = llvm.insertvalue %66, %65[4, 1] : !llvm.struct<(ptr, ptr, i64, array<2 x i64>, array<2 x i64>)>
    %68 = llvm.extractvalue %15[0] : !llvm.struct<(ptr, ptr, i64, array<2 x i64>, array<2 x i64>)>
    %69 = llvm.extractvalue %15[1] : !llvm.struct<(ptr, ptr, i64, array<2 x i64>, array<2 x i64>)>
    %70 = llvm.mlir.poison : !llvm.struct<(ptr, ptr, i64)>
    %71 = llvm.insertvalue %68, %70[0] : !llvm.struct<(ptr, ptr, i64)>
    %72 = llvm.insertvalue %69, %71[1] : !llvm.struct<(ptr, ptr, i64)>
    %73 = llvm.mlir.constant(0 : index) : i64
    %74 = llvm.insertvalue %73, %72[2] : !llvm.struct<(ptr, ptr, i64)>
    %75 = llvm.extractvalue %15[2] : !llvm.struct<(ptr, ptr, i64, array<2 x i64>, array<2 x i64>)>
    %76 = llvm.extractvalue %15[3, 0] : !llvm.struct<(ptr, ptr, i64, array<2 x i64>, array<2 x i64>)>
    %77 = llvm.extractvalue %15[3, 1] : !llvm.struct<(ptr, ptr, i64, array<2 x i64>, array<2 x i64>)>
    %78 = llvm.extractvalue %15[4, 0] : !llvm.struct<(ptr, ptr, i64, array<2 x i64>, array<2 x i64>)>
    %79 = llvm.extractvalue %15[4, 1] : !llvm.struct<(ptr, ptr, i64, array<2 x i64>, array<2 x i64>)>
    %80 = llvm.mlir.constant(16 : index) : i64
    %81 = llvm.mul %35, %80 overflow<nsw> : i64
    %82 = llvm.add %81, %37 : i64
    %83 = llvm.mlir.poison : !llvm.struct<(ptr, ptr, i64, array<2 x i64>, array<2 x i64>)>
    %84 = llvm.extractvalue %74[0] : !llvm.struct<(ptr, ptr, i64)>
    %85 = llvm.extractvalue %74[1] : !llvm.struct<(ptr, ptr, i64)>
    %86 = llvm.insertvalue %84, %83[0] : !llvm.struct<(ptr, ptr, i64, array<2 x i64>, array<2 x i64>)>
    %87 = llvm.insertvalue %85, %86[1] : !llvm.struct<(ptr, ptr, i64, array<2 x i64>, array<2 x i64>)>
    %88 = llvm.insertvalue %82, %87[2] : !llvm.struct<(ptr, ptr, i64, array<2 x i64>, array<2 x i64>)>
    %89 = llvm.mlir.constant(4 : index) : i64
    %90 = llvm.insertvalue %89, %88[3, 0] : !llvm.struct<(ptr, ptr, i64, array<2 x i64>, array<2 x i64>)>
    %91 = llvm.mlir.constant(16 : index) : i64
    %92 = llvm.insertvalue %91, %90[4, 0] : !llvm.struct<(ptr, ptr, i64, array<2 x i64>, array<2 x i64>)>
    %93 = llvm.mlir.constant(4 : index) : i64
    %94 = llvm.insertvalue %93, %92[3, 1] : !llvm.struct<(ptr, ptr, i64, array<2 x i64>, array<2 x i64>)>
    %95 = llvm.mlir.constant(1 : index) : i64
    %96 = llvm.insertvalue %95, %94[4, 1] : !llvm.struct<(ptr, ptr, i64, array<2 x i64>, array<2 x i64>)>
    %97 = llvm.extractvalue %7[0] : !llvm.struct<(ptr, ptr, i64, array<2 x i64>, array<2 x i64>)>
    %98 = llvm.extractvalue %7[1] : !llvm.struct<(ptr, ptr, i64, array<2 x i64>, array<2 x i64>)>
    %99 = llvm.mlir.poison : !llvm.struct<(ptr, ptr, i64)>
    %100 = llvm.insertvalue %97, %99[0] : !llvm.struct<(ptr, ptr, i64)>
    %101 = llvm.insertvalue %98, %100[1] : !llvm.struct<(ptr, ptr, i64)>
    %102 = llvm.mlir.constant(0 : index) : i64
    %103 = llvm.insertvalue %102, %101[2] : !llvm.struct<(ptr, ptr, i64)>
    %104 = llvm.extractvalue %7[2] : !llvm.struct<(ptr, ptr, i64, array<2 x i64>, array<2 x i64>)>
    %105 = llvm.extractvalue %7[3, 0] : !llvm.struct<(ptr, ptr, i64, array<2 x i64>, array<2 x i64>)>
    %106 = llvm.extractvalue %7[3, 1] : !llvm.struct<(ptr, ptr, i64, array<2 x i64>, array<2 x i64>)>
    %107 = llvm.extractvalue %7[4, 0] : !llvm.struct<(ptr, ptr, i64, array<2 x i64>, array<2 x i64>)>
    %108 = llvm.extractvalue %7[4, 1] : !llvm.struct<(ptr, ptr, i64, array<2 x i64>, array<2 x i64>)>
    %109 = llvm.mlir.constant(16 : index) : i64
    %110 = llvm.mul %33, %109 overflow<nsw> : i64
    %111 = llvm.add %110, %37 : i64
    %112 = llvm.mlir.poison : !llvm.struct<(ptr, ptr, i64, array<2 x i64>, array<2 x i64>)>
    %113 = llvm.extractvalue %103[0] : !llvm.struct<(ptr, ptr, i64)>
    %114 = llvm.extractvalue %103[1] : !llvm.struct<(ptr, ptr, i64)>
    %115 = llvm.insertvalue %113, %112[0] : !llvm.struct<(ptr, ptr, i64, array<2 x i64>, array<2 x i64>)>
    %116 = llvm.insertvalue %114, %115[1] : !llvm.struct<(ptr, ptr, i64, array<2 x i64>, array<2 x i64>)>
    %117 = llvm.insertvalue %111, %116[2] : !llvm.struct<(ptr, ptr, i64, array<2 x i64>, array<2 x i64>)>
    %118 = llvm.mlir.constant(4 : index) : i64
    %119 = llvm.insertvalue %118, %117[3, 0] : !llvm.struct<(ptr, ptr, i64, array<2 x i64>, array<2 x i64>)>
    %120 = llvm.mlir.constant(16 : index) : i64
    %121 = llvm.insertvalue %120, %119[4, 0] : !llvm.struct<(ptr, ptr, i64, array<2 x i64>, array<2 x i64>)>
    %122 = llvm.mlir.constant(4 : index) : i64
    %123 = llvm.insertvalue %122, %121[3, 1] : !llvm.struct<(ptr, ptr, i64, array<2 x i64>, array<2 x i64>)>
    %124 = llvm.mlir.constant(1 : index) : i64
    %125 = llvm.insertvalue %124, %123[4, 1] : !llvm.struct<(ptr, ptr, i64, array<2 x i64>, array<2 x i64>)>
    llvm.br ^bb7(%30 : i64)
  ^bb7(%126: i64):  // 2 preds: ^bb6, ^bb14
    %127 = llvm.icmp "slt" %126, %32 : i64
    llvm.cond_br %127, ^bb8, ^bb15
  ^bb8:  // pred: ^bb7
    llvm.br ^bb9(%30 : i64)
  ^bb9(%128: i64):  // 2 preds: ^bb8, ^bb13
    %129 = llvm.icmp "slt" %128, %32 : i64
    llvm.cond_br %129, ^bb10, ^bb14
  ^bb10:  // pred: ^bb9
    llvm.br ^bb11(%30 : i64)
  ^bb11(%130: i64):  // 2 preds: ^bb10, ^bb12
    %131 = llvm.icmp "slt" %130, %32 : i64
    llvm.cond_br %131, ^bb12, ^bb13
  ^bb12:  // pred: ^bb11
    %132 = llvm.mlir.constant(16 : index) : i64
    %133 = llvm.mul %33, %132 overflow<nsw> : i64
    %134 = llvm.add %133, %35 : i64
    %135 = llvm.mul %126, %31 : i64
    %136 = llvm.add %134, %135 : i64
    %137 = llvm.add %136, %130 : i64
    %138 = llvm.mul %137, %32 : i64
    llvm.call @__cachescope_trace_v1(%28, %28, %138, %27, %28) : (i64, i64, i64, i64, i64) -> ()
    %139 = llvm.extractvalue %67[1] : !llvm.struct<(ptr, ptr, i64, array<2 x i64>, array<2 x i64>)>
    %140 = llvm.extractvalue %67[2] : !llvm.struct<(ptr, ptr, i64, array<2 x i64>, array<2 x i64>)>
    %141 = llvm.getelementptr %139[%140] : (!llvm.ptr, i64) -> !llvm.ptr, f32
    %142 = llvm.mlir.constant(16 : index) : i64
    %143 = llvm.mul %126, %142 overflow<nsw, nuw> : i64
    %144 = llvm.add %143, %130 overflow<nsw, nuw> : i64
    %145 = llvm.getelementptr inbounds|nuw %141[%144] : (!llvm.ptr, i64) -> !llvm.ptr, f32
    %146 = llvm.load %145 : !llvm.ptr -> f32
    %147 = llvm.mlir.constant(16 : index) : i64
    %148 = llvm.mul %35, %147 overflow<nsw> : i64
    %149 = llvm.add %148, %37 : i64
    %150 = llvm.mul %130, %31 : i64
    %151 = llvm.add %149, %150 : i64
    %152 = llvm.add %151, %128 : i64
    %153 = llvm.mul %152, %32 : i64
    llvm.call @__cachescope_trace_v1(%26, %26, %153, %27, %28) : (i64, i64, i64, i64, i64) -> ()
    %154 = llvm.extractvalue %96[1] : !llvm.struct<(ptr, ptr, i64, array<2 x i64>, array<2 x i64>)>
    %155 = llvm.extractvalue %96[2] : !llvm.struct<(ptr, ptr, i64, array<2 x i64>, array<2 x i64>)>
    %156 = llvm.getelementptr %154[%155] : (!llvm.ptr, i64) -> !llvm.ptr, f32
    %157 = llvm.mlir.constant(16 : index) : i64
    %158 = llvm.mul %130, %157 overflow<nsw, nuw> : i64
    %159 = llvm.add %158, %128 overflow<nsw, nuw> : i64
    %160 = llvm.getelementptr inbounds|nuw %156[%159] : (!llvm.ptr, i64) -> !llvm.ptr, f32
    %161 = llvm.load %160 : !llvm.ptr -> f32
    %162 = llvm.mlir.constant(16 : index) : i64
    %163 = llvm.mul %33, %162 overflow<nsw> : i64
    %164 = llvm.add %163, %37 : i64
    %165 = llvm.add %164, %135 : i64
    %166 = llvm.add %165, %128 : i64
    %167 = llvm.mul %166, %32 : i64
    llvm.call @__cachescope_trace_v1(%25, %25, %167, %27, %28) : (i64, i64, i64, i64, i64) -> ()
    %168 = llvm.extractvalue %125[1] : !llvm.struct<(ptr, ptr, i64, array<2 x i64>, array<2 x i64>)>
    %169 = llvm.extractvalue %125[2] : !llvm.struct<(ptr, ptr, i64, array<2 x i64>, array<2 x i64>)>
    %170 = llvm.getelementptr %168[%169] : (!llvm.ptr, i64) -> !llvm.ptr, f32
    %171 = llvm.mlir.constant(16 : index) : i64
    %172 = llvm.mul %126, %171 overflow<nsw, nuw> : i64
    %173 = llvm.add %172, %128 overflow<nsw, nuw> : i64
    %174 = llvm.getelementptr inbounds|nuw %170[%173] : (!llvm.ptr, i64) -> !llvm.ptr, f32
    %175 = llvm.load %174 : !llvm.ptr -> f32
    %176 = llvm.fmul %146, %161 : f32
    %177 = llvm.fadd %175, %176 : f32
    llvm.call @__cachescope_trace_v1(%24, %25, %167, %27, %26) : (i64, i64, i64, i64, i64) -> ()
    %178 = llvm.extractvalue %125[1] : !llvm.struct<(ptr, ptr, i64, array<2 x i64>, array<2 x i64>)>
    %179 = llvm.extractvalue %125[2] : !llvm.struct<(ptr, ptr, i64, array<2 x i64>, array<2 x i64>)>
    %180 = llvm.getelementptr %178[%179] : (!llvm.ptr, i64) -> !llvm.ptr, f32
    %181 = llvm.mlir.constant(16 : index) : i64
    %182 = llvm.mul %126, %181 overflow<nsw, nuw> : i64
    %183 = llvm.add %182, %128 overflow<nsw, nuw> : i64
    %184 = llvm.getelementptr inbounds|nuw %180[%183] : (!llvm.ptr, i64) -> !llvm.ptr, f32
    llvm.store %177, %184 : f32, !llvm.ptr
    %185 = llvm.add %130, %29 : i64
    llvm.br ^bb11(%185 : i64)
  ^bb13:  // pred: ^bb11
    %186 = llvm.add %128, %29 : i64
    llvm.br ^bb9(%186 : i64)
  ^bb14:  // pred: ^bb9
    %187 = llvm.add %126, %29 : i64
    llvm.br ^bb7(%187 : i64)
  ^bb15:  // pred: ^bb7
    %188 = llvm.add %37, %32 : i64
    llvm.br ^bb5(%188 : i64)
  ^bb16:  // pred: ^bb5
    %189 = llvm.add %35, %32 : i64
    llvm.br ^bb3(%189 : i64)
  ^bb17:  // pred: ^bb3
    %190 = llvm.add %33, %32 : i64
    llvm.br ^bb1(%190 : i64)
  ^bb18:  // pred: ^bb1
    llvm.return
  }
  llvm.func @main() -> i32 {
    %0 = llvm.mlir.constant(1.000000e+00 : f32) : f32
    %1 = llvm.mlir.constant(0.000000e+00 : f32) : f32
    %2 = llvm.mlir.constant(16 : index) : i64
    %3 = llvm.mlir.constant(1 : index) : i64
    %4 = llvm.mlir.constant(0 : index) : i64
    %5 = llvm.mlir.constant(16 : index) : i64
    %6 = llvm.mlir.constant(16 : index) : i64
    %7 = llvm.mlir.constant(1 : index) : i64
    %8 = llvm.mlir.constant(256 : index) : i64
    %9 = llvm.alloca %8 x f32 : (i64) -> !llvm.ptr
    %10 = llvm.mlir.poison : !llvm.struct<(ptr, ptr, i64, array<2 x i64>, array<2 x i64>)>
    %11 = llvm.insertvalue %9, %10[0] : !llvm.struct<(ptr, ptr, i64, array<2 x i64>, array<2 x i64>)>
    %12 = llvm.insertvalue %9, %11[1] : !llvm.struct<(ptr, ptr, i64, array<2 x i64>, array<2 x i64>)>
    %13 = llvm.mlir.constant(0 : index) : i64
    %14 = llvm.insertvalue %13, %12[2] : !llvm.struct<(ptr, ptr, i64, array<2 x i64>, array<2 x i64>)>
    %15 = llvm.insertvalue %5, %14[3, 0] : !llvm.struct<(ptr, ptr, i64, array<2 x i64>, array<2 x i64>)>
    %16 = llvm.insertvalue %6, %15[3, 1] : !llvm.struct<(ptr, ptr, i64, array<2 x i64>, array<2 x i64>)>
    %17 = llvm.insertvalue %6, %16[4, 0] : !llvm.struct<(ptr, ptr, i64, array<2 x i64>, array<2 x i64>)>
    %18 = llvm.insertvalue %7, %17[4, 1] : !llvm.struct<(ptr, ptr, i64, array<2 x i64>, array<2 x i64>)>
    %19 = llvm.mlir.constant(16 : index) : i64
    %20 = llvm.mlir.constant(16 : index) : i64
    %21 = llvm.mlir.constant(1 : index) : i64
    %22 = llvm.mlir.constant(256 : index) : i64
    %23 = llvm.alloca %22 x f32 : (i64) -> !llvm.ptr
    %24 = llvm.mlir.poison : !llvm.struct<(ptr, ptr, i64, array<2 x i64>, array<2 x i64>)>
    %25 = llvm.insertvalue %23, %24[0] : !llvm.struct<(ptr, ptr, i64, array<2 x i64>, array<2 x i64>)>
    %26 = llvm.insertvalue %23, %25[1] : !llvm.struct<(ptr, ptr, i64, array<2 x i64>, array<2 x i64>)>
    %27 = llvm.mlir.constant(0 : index) : i64
    %28 = llvm.insertvalue %27, %26[2] : !llvm.struct<(ptr, ptr, i64, array<2 x i64>, array<2 x i64>)>
    %29 = llvm.insertvalue %19, %28[3, 0] : !llvm.struct<(ptr, ptr, i64, array<2 x i64>, array<2 x i64>)>
    %30 = llvm.insertvalue %20, %29[3, 1] : !llvm.struct<(ptr, ptr, i64, array<2 x i64>, array<2 x i64>)>
    %31 = llvm.insertvalue %20, %30[4, 0] : !llvm.struct<(ptr, ptr, i64, array<2 x i64>, array<2 x i64>)>
    %32 = llvm.insertvalue %21, %31[4, 1] : !llvm.struct<(ptr, ptr, i64, array<2 x i64>, array<2 x i64>)>
    %33 = llvm.mlir.constant(16 : index) : i64
    %34 = llvm.mlir.constant(16 : index) : i64
    %35 = llvm.mlir.constant(1 : index) : i64
    %36 = llvm.mlir.constant(256 : index) : i64
    %37 = llvm.alloca %36 x f32 : (i64) -> !llvm.ptr
    %38 = llvm.mlir.poison : !llvm.struct<(ptr, ptr, i64, array<2 x i64>, array<2 x i64>)>
    %39 = llvm.insertvalue %37, %38[0] : !llvm.struct<(ptr, ptr, i64, array<2 x i64>, array<2 x i64>)>
    %40 = llvm.insertvalue %37, %39[1] : !llvm.struct<(ptr, ptr, i64, array<2 x i64>, array<2 x i64>)>
    %41 = llvm.mlir.constant(0 : index) : i64
    %42 = llvm.insertvalue %41, %40[2] : !llvm.struct<(ptr, ptr, i64, array<2 x i64>, array<2 x i64>)>
    %43 = llvm.insertvalue %33, %42[3, 0] : !llvm.struct<(ptr, ptr, i64, array<2 x i64>, array<2 x i64>)>
    %44 = llvm.insertvalue %34, %43[3, 1] : !llvm.struct<(ptr, ptr, i64, array<2 x i64>, array<2 x i64>)>
    %45 = llvm.insertvalue %34, %44[4, 0] : !llvm.struct<(ptr, ptr, i64, array<2 x i64>, array<2 x i64>)>
    %46 = llvm.insertvalue %35, %45[4, 1] : !llvm.struct<(ptr, ptr, i64, array<2 x i64>, array<2 x i64>)>
    llvm.br ^bb1(%4 : i64)
  ^bb1(%47: i64):  // 2 preds: ^bb0, ^bb5
    %48 = llvm.icmp "slt" %47, %2 : i64
    llvm.cond_br %48, ^bb2, ^bb6
  ^bb2:  // pred: ^bb1
    llvm.br ^bb3(%4 : i64)
  ^bb3(%49: i64):  // 2 preds: ^bb2, ^bb4
    %50 = llvm.icmp "slt" %49, %2 : i64
    llvm.cond_br %50, ^bb4, ^bb5
  ^bb4:  // pred: ^bb3
    %51 = llvm.extractvalue %18[1] : !llvm.struct<(ptr, ptr, i64, array<2 x i64>, array<2 x i64>)>
    %52 = llvm.mlir.constant(16 : index) : i64
    %53 = llvm.mul %47, %52 overflow<nsw, nuw> : i64
    %54 = llvm.add %53, %49 overflow<nsw, nuw> : i64
    %55 = llvm.getelementptr inbounds|nuw %51[%54] : (!llvm.ptr, i64) -> !llvm.ptr, f32
    llvm.store %0, %55 : f32, !llvm.ptr
    %56 = llvm.extractvalue %32[1] : !llvm.struct<(ptr, ptr, i64, array<2 x i64>, array<2 x i64>)>
    %57 = llvm.mlir.constant(16 : index) : i64
    %58 = llvm.mul %47, %57 overflow<nsw, nuw> : i64
    %59 = llvm.add %58, %49 overflow<nsw, nuw> : i64
    %60 = llvm.getelementptr inbounds|nuw %56[%59] : (!llvm.ptr, i64) -> !llvm.ptr, f32
    llvm.store %0, %60 : f32, !llvm.ptr
    %61 = llvm.extractvalue %46[1] : !llvm.struct<(ptr, ptr, i64, array<2 x i64>, array<2 x i64>)>
    %62 = llvm.mlir.constant(16 : index) : i64
    %63 = llvm.mul %47, %62 overflow<nsw, nuw> : i64
    %64 = llvm.add %63, %49 overflow<nsw, nuw> : i64
    %65 = llvm.getelementptr inbounds|nuw %61[%64] : (!llvm.ptr, i64) -> !llvm.ptr, f32
    llvm.store %1, %65 : f32, !llvm.ptr
    %66 = llvm.add %49, %3 : i64
    llvm.br ^bb3(%66 : i64)
  ^bb5:  // pred: ^bb3
    %67 = llvm.add %47, %3 : i64
    llvm.br ^bb1(%67 : i64)
  ^bb6:  // pred: ^bb1
    %68 = llvm.extractvalue %18[0] : !llvm.struct<(ptr, ptr, i64, array<2 x i64>, array<2 x i64>)>
    %69 = llvm.extractvalue %18[1] : !llvm.struct<(ptr, ptr, i64, array<2 x i64>, array<2 x i64>)>
    %70 = llvm.extractvalue %18[2] : !llvm.struct<(ptr, ptr, i64, array<2 x i64>, array<2 x i64>)>
    %71 = llvm.extractvalue %18[3, 0] : !llvm.struct<(ptr, ptr, i64, array<2 x i64>, array<2 x i64>)>
    %72 = llvm.extractvalue %18[3, 1] : !llvm.struct<(ptr, ptr, i64, array<2 x i64>, array<2 x i64>)>
    %73 = llvm.extractvalue %18[4, 0] : !llvm.struct<(ptr, ptr, i64, array<2 x i64>, array<2 x i64>)>
    %74 = llvm.extractvalue %18[4, 1] : !llvm.struct<(ptr, ptr, i64, array<2 x i64>, array<2 x i64>)>
    %75 = llvm.extractvalue %32[0] : !llvm.struct<(ptr, ptr, i64, array<2 x i64>, array<2 x i64>)>
    %76 = llvm.extractvalue %32[1] : !llvm.struct<(ptr, ptr, i64, array<2 x i64>, array<2 x i64>)>
    %77 = llvm.extractvalue %32[2] : !llvm.struct<(ptr, ptr, i64, array<2 x i64>, array<2 x i64>)>
    %78 = llvm.extractvalue %32[3, 0] : !llvm.struct<(ptr, ptr, i64, array<2 x i64>, array<2 x i64>)>
    %79 = llvm.extractvalue %32[3, 1] : !llvm.struct<(ptr, ptr, i64, array<2 x i64>, array<2 x i64>)>
    %80 = llvm.extractvalue %32[4, 0] : !llvm.struct<(ptr, ptr, i64, array<2 x i64>, array<2 x i64>)>
    %81 = llvm.extractvalue %32[4, 1] : !llvm.struct<(ptr, ptr, i64, array<2 x i64>, array<2 x i64>)>
    %82 = llvm.extractvalue %46[0] : !llvm.struct<(ptr, ptr, i64, array<2 x i64>, array<2 x i64>)>
    %83 = llvm.extractvalue %46[1] : !llvm.struct<(ptr, ptr, i64, array<2 x i64>, array<2 x i64>)>
    %84 = llvm.extractvalue %46[2] : !llvm.struct<(ptr, ptr, i64, array<2 x i64>, array<2 x i64>)>
    %85 = llvm.extractvalue %46[3, 0] : !llvm.struct<(ptr, ptr, i64, array<2 x i64>, array<2 x i64>)>
    %86 = llvm.extractvalue %46[3, 1] : !llvm.struct<(ptr, ptr, i64, array<2 x i64>, array<2 x i64>)>
    %87 = llvm.extractvalue %46[4, 0] : !llvm.struct<(ptr, ptr, i64, array<2 x i64>, array<2 x i64>)>
    %88 = llvm.extractvalue %46[4, 1] : !llvm.struct<(ptr, ptr, i64, array<2 x i64>, array<2 x i64>)>
    llvm.call @matmul_kernel(%68, %69, %70, %71, %72, %73, %74, %75, %76, %77, %78, %79, %80, %81, %82, %83, %84, %85, %86, %87, %88) : (!llvm.ptr, !llvm.ptr, i64, i64, i64, i64, i64, !llvm.ptr, !llvm.ptr, i64, i64, i64, i64, i64, !llvm.ptr, !llvm.ptr, i64, i64, i64, i64, i64) -> ()
    %89 = llvm.extractvalue %46[1] : !llvm.struct<(ptr, ptr, i64, array<2 x i64>, array<2 x i64>)>
    %90 = llvm.mlir.constant(16 : index) : i64
    %91 = llvm.mul %4, %90 overflow<nsw, nuw> : i64
    %92 = llvm.add %91, %4 overflow<nsw, nuw> : i64
    %93 = llvm.getelementptr inbounds|nuw %89[%92] : (!llvm.ptr, i64) -> !llvm.ptr, f32
    %94 = llvm.load %93 : !llvm.ptr -> f32
    %95 = llvm.fptosi %94 : f32 to i32
    llvm.return %95 : i32
  }
}
