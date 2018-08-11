
#ifndef VPIC_ACCUMULATOR_H
#define VPIC_ACCUMULATOR_H

template<typename Accumulator, typename FieldArray>
struct VpicAccumulatorOps
{
  static void clear(Accumulator& acc)                        { ::clear_accumulator_array(&acc); }
  static void reduce(Accumulator& acc)                       { ::reduce_accumulator_array(&acc); }
  static void unload(const Accumulator& acc, FieldArray* fa) { ::unload_accumulator_array(&fa, &acc); }
};

template<class AccumulatorBase, class FieldArray>
struct VpicAccumulator : AccumulatorBase
{
};

#endif
