// Copyright 2020 The TensorFlow Runtime Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

//===- data_ops.td --------------------------------------------------------===//
//
// Operation definitions for data_ops.
//
//===----------------------------------------------------------------------===//

#ifdef DATA_OPS
#else
#define DATA_OPS

include "tfrt/tfrt_op_base.td"

// "data" dialect
def Data_Dialect : Dialect {
  let name = "data";

  let description = [{
    The Data dialect.

    This dialect contains common data operations.
  }];

  let cppNamespace = "data";
}

// Type definition.
//
// TODO(tfrt-dev): Define these types properly.
def DatasetType: OpaqueType<"tfrt", "dataset", "!tfrt.dataset type">;
def IteratorType: OpaqueType<"tfrt", "iterator", "!tfrt.iterator type">;

// Base class for Data dialect ops.
class Data_Op<string mnemonic, list<OpTrait> traits = []> :
    Op<Data_Dialect, mnemonic, !listconcat(traits, [IsolatedFromAbove])> {

  // Each registered op in the Data namespace needs to provide a parser.
  let parser = [{ return tfrt::data::parse$cppClass(parser, result); }];
}

//===----------------------------------------------------------------------===//
// Iterator ops
//===----------------------------------------------------------------------===//

def MakeIteratorOp : Data_Op<"make_iterator"> {
  let summary = "data make_iterator operation";
  let description = [{
    data.make_iterator creates an iterator from a dataset.

    Example:
      %iterator = data.make_iterator %dataset
  }];

  let arguments = (ins DatasetType:$dataset);
  let results = (outs IteratorType:$iterator);

  let assemblyFormat = "operands attr-dict";
}

def IteratorGetNextOp : Data_Op<"iterator_get_next"> {
  let summary = "data iterator_get_next operation";
  let description = [{
    data.iterator_get_next yields a chain and an element from the iterator.

    Example:
      %iterator = data.make_iterator %dataset
      %chain_out, %value = data.iterator_get_next (%iterator, %chain_in) : !t.tensor
  }];

  let arguments = (ins IteratorType:$iterator, TFRT_ChainType:$chain_in);
  let results = (outs Variadic<AnyType>:$results);
}

def EnumerateIteratorOp : Data_Op<"enumerate.iterator"> {
  let summary = "data enumerate.iterator operation";
  let description = [{
    data.enumerate.iterator repeatedly draws the next element from the iterator
    and executes the function on it to update the arguments until the iterator
    is exhausted.

    The result types are specified after the colon. The results have the same
    types as the operands, excluding the first operand, which is the iterator.

    example:
      data.enumerate.iterator (%iterator, %zero) { function = @add_one } : i64
  }];

  let arguments = (ins
    IteratorType:$iterator,
    Variadic<AnyType>:$arguments,

    FlatSymbolRefAttr:$function
  );

  let results = (outs Variadic<AnyType>:$results);
  let verifier = [{ return tfrt::data::verify(*this); }];
}

//===----------------------------------------------------------------------===//
// Dataset ops
//===----------------------------------------------------------------------===//

class BatchDatasetOp<string suffix>
  : Data_Op<"batch_dataset." # suffix> {
  let summary = "data batch_dataset operation";
  let description = [{
    data.batch_dataset wraps around another dataset instance and batches the
    underlying elements.

    Example:
      %dataset1 = data.range_dataset %start, %stop, %step {element_type = i32}
      %batch_size = tfrt.constant.i64 32
      %dataset2 = data.batch_dataset.i32 %dataset1, %batch_size
        { same_input_metadata = 1 : i1 }
  }];

  let arguments = (ins
     DatasetType:$input_dataset,
     I64:$batch_size,

     I1Attr:$same_input_metadata
  );
  let results = (outs DatasetType:$output_dataset);

  let assemblyFormat = "operands attr-dict";
}

// TODO(b/155892156): In the long term, dataset ops will not be specialized
// for types. Instead, type information will be passed in via attributes.
def BatchDatasetI32Op : BatchDatasetOp<"i32">;
def BatchDatasetI64Op : BatchDatasetOp<"i64">;
def BatchDatasetTensorOp : BatchDatasetOp<"tensor">;
def BatchDatasetTensorAndI64Op : BatchDatasetOp<"tensor_and_i64">;

// TODO(rachelim): Add verification to filter functions.
def FilterDatasetOp : Data_Op<"filter_dataset"> {
  let summary = "data filter_dataset operation";
  let description = [{
    data.filter_dataset takes elements from the input dataset and yields the
    elements which satisfy a user-defined filter function.

    Example:
      %dataset1 = data.range_dataset.i32 (%start, %stop, %step)
      %dataset2 = data.filter_dataset (%dataset1)
        {function = @filter_even, arity = 1: i64}
  }];

  let arguments = (ins
    DatasetType:$input_dataset,

    I64Attr:$arity,
    FlatSymbolRefAttr:$function
  );

  let results = (outs DatasetType:$output_dataset);

  let assemblyFormat = "operands attr-dict";
}

// TODO(rachelim): Add verification to interleave functions.
def InterleaveDatasetOp : Data_Op<"interleave_dataset"> {
  let summary = "data interleave_dataset operation";
  let description = [{
    data.interleave_dataset applies a function to its input to create a dataset
    per input elements and interleaves the results of these datasets.

    Example:
      %dataset = data.interleave_dataset (%filenames)
        {function = @read_tf_record_files, arity = 1: i64}
  }];

  let arguments = (ins
    DatasetType:$input_dataset,
    I64:$cycle_length,
    I64:$block_length,

    I64Attr:$arity,
    FlatSymbolRefAttr:$function
  );

  let results = (outs DatasetType:$output_dataset);

  let assemblyFormat = "operands attr-dict";
}

// TODO(rachelim): Add verification to map functions.
def MapDatasetOp : Data_Op<"map_dataset"> {
  let summary = "data map_dataset operation";
  let description = [{
    data.map_dataset maps a user-defined function over the elements in its
    input dataset.

    Example:
      %dataset1 = data.range_dataset %start, %stop, %step {element_type = i32}
      %dataset2 = data.map_dataset %dataset1 {function = @times_two}
  }];

  let arguments = (ins
    DatasetType:$input_dataset,
    Variadic<AnyType>:$other_arguments,

    FlatSymbolRefAttr:$function
  );

  let results = (outs DatasetType:$output_dataset);

  let assemblyFormat = [{
    $input_dataset (`,` $other_arguments^ `:` type($other_arguments))?
    attr-dict
  }];
}

def PrefetchDatasetOp : Data_Op<"prefetch_dataset"> {
  let summary = "data prefetch_dataset operation";
  let description = [{
    data.prefetch_dataset wraps around another dataset instance and prefetches
    elements from the underlying dataset in an internal buffer.

    Example:
      %dataset1 = data.range_dataset %start, %stop, %step {element_type = i32}
      %dataset2 = data.map_dataset %dataset1 {function = @times_two}
      %prefetch_num = tfrt.constant.i64 8
      %dataset3 = data.prefetch_dataset %dataset2, %prefetch_num
  }];

  let arguments = (ins
    DatasetType:$input_dataset,
    I64:$prefetch_num
  );

  let results = (outs DatasetType:$output_dataset);

  let assemblyFormat = "operands attr-dict";
}

def RangeDatasetOp : Data_Op<"range_dataset" > {
  let summary = "data range_dataset operation";
  let description = [{
    data.range_dataset yields a step-separated range of values from
    start (inclusive) to stop (exclusive).

    Example:
      %dataset = data.range_dataset %start, %stop, %step {element_type = i32}
  }];

  let arguments = (ins
     I64:$start,
     I64:$stop,
     I64:$step,

     TypeAttr:$element_type
  );
  let results = (outs DatasetType:$output_dataset);
  let verifier = [{ return tfrt::data::verify(*this); }];
  let assemblyFormat = "operands attr-dict";
}

def RepeatDatasetOp : Data_Op<"repeat_dataset"> {
  let summary = "data repeat_dataset operation";
  let description = [{
    data.repeat_dataset wraps around another dataset instance and repeats
    it a specified number of times.

    Example:
      %dataset1 = data.range_dataset %start, %stop, %step {element_type = i32}
      %count = tfrt.constant.i64 8
      %dataset2 = data.repeat_dataset %dataset1, %count
  }];

  let arguments = (ins
    DatasetType:$input_dataset,
    I64:$count
  );

  let results = (outs DatasetType:$output_dataset);

  let assemblyFormat = "operands attr-dict";
}

def TFRecordDatasetOp : Data_Op<"tf_record_dataset"> {
  let summary = "data tf_record_dataset operation";
  let description = [{
    data.tf_record_dataset reads TFRecord bytes from a file.

    Example:
      %dataset = data.tf_record_dataset %path
  }];

  let arguments = (ins
    StringType:$path
  );

  let results = (outs DatasetType:$output_dataset);

  let assemblyFormat = "operands attr-dict";
}

#endif  // DATA_OPS
