#include "passthrough_operator.h"

namespace flowsql {

int PassthroughOperator::Work(IDataFrame* in, IDataFrame* out) {
    out->SetSchema(in->GetSchema());
    for (int32_t i = 0; i < in->RowCount(); ++i) {
        out->AppendRow(in->GetRow(i));
    }
    return 0;
}

}  // namespace flowsql
