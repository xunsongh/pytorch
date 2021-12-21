#include <torch/csrc/jit/codegen/cuda/lower_utils.h>

#include <ATen/cuda/CUDAContext.h>
#include <c10/util/irange.h>
#include <torch/csrc/jit/codegen/cuda/arith.h>
#include <torch/csrc/jit/codegen/cuda/ir_iostream.h>
#include <torch/csrc/jit/codegen/cuda/ir_utils.h>
#include <torch/csrc/jit/codegen/cuda/iter_visitor.h>
#include <torch/csrc/jit/codegen/cuda/kernel_ir_builder.h>
#include <torch/csrc/jit/codegen/cuda/kernel_ir_printer.h>
#include <torch/csrc/jit/codegen/cuda/lower2device.h>
#include <torch/csrc/jit/codegen/cuda/lower_thread_predicate.h>
#include <torch/csrc/jit/codegen/cuda/root_domain_map.h>

#include <algorithm>

// TODO: refactor this file (one per namespace)

namespace torch {
namespace jit {
namespace fuser {
namespace cuda {

namespace scope_utils {

// TODO: Factor this out of lower_index.cpp and remove if possible
std::vector<kir::ForLoop*> getLoops(kir::Expr* scope) {
  std::vector<kir::ForLoop*> loops;
  while (scope != nullptr) {
    if (auto loop = dynamic_cast<kir::ForLoop*>(scope)) {
      loops.push_back(loop);
    }
    scope = scope->parentScope();
  }
  std::reverse(loops.begin(), loops.end());
  return loops;
}

//! Create an **empty** Forloop and copy the metadata.
kir::ForLoop* cloneForLoop(kir::IrBuilder& ir_builder, kir::ForLoop* for_loop) {
  return ir_builder.create<kir::ForLoop>(for_loop);
}

//! Create an **empty** IfThenElse and copy the metadata.
kir::IfThenElse* cloneIfThenElse(
    kir::IrBuilder& ir_builder,
    kir::IfThenElse* ite) {
  return ir_builder.create<kir::IfThenElse>(ite->predicate());
}

} // namespace scope_utils

namespace ir_utils {

TVDomainGuard::TVDomainGuard(TensorView* _tv, TensorDomain* td)
    : tv_(_tv), prev_domain(tv_->domain()) {
  tv_->setDomain(td);
}

TVDomainGuard::~TVDomainGuard() {
  tv_->setDomain(prev_domain);
}

std::vector<IterDomain*> iterDomainInputsOf(
    const std::vector<IterDomain*>& input_ids,
    const std::vector<IterDomain*>& all_inputs) {
  auto inputs = IterVisitor::getInputsTo(
      {input_ids.begin(), input_ids.end()},
      {all_inputs.begin(), all_inputs.end()});
  std::vector<IterDomain*> id_inputs(
      ir_utils::filterByType<IterDomain>(inputs).begin(),
      ir_utils::filterByType<IterDomain>(inputs).end());
  return id_inputs;
}

std::vector<IterDomain*> iterDomainInputsOfOrderedAs(
    const std::vector<IterDomain*>& of,
    const std::vector<IterDomain*>& order) {
  auto inputs_vec = iterDomainInputsOf(of, order);

  std::unordered_set<IterDomain*> inputs_set(
      inputs_vec.begin(), inputs_vec.end());

  std::vector<IterDomain*> ordered_inputs;
  std::copy_if(
      order.begin(),
      order.end(),
      std::back_inserter(ordered_inputs),
      [&inputs_set](const auto& id) {
        return inputs_set.find(id) != inputs_set.end();
      });

  return ordered_inputs;
}

bool isTV(const Val* val) {
  return val->getValType().value() == ValType::TensorView;
}

// Check if we're a TensorView op that we can generate code for.
bool isTVOp(const Expr* expr) {
  if (std::any_of(
          expr->outputs().begin(),
          expr->outputs().end(),
          [](Val* v) { return isTV(v); }) &&
      (expr->getExprType().value() == ExprType::BinaryOp ||
       expr->getExprType().value() == ExprType::UnaryOp ||
       expr->getExprType().value() == ExprType::TernaryOp ||
       expr->getExprType().value() == ExprType::ReductionOp ||
       expr->getExprType().value() == ExprType::WelfordOp ||
       expr->getExprType().value() == ExprType::BroadcastOp ||
       expr->getExprType().value() == ExprType::TransposeOp ||
       expr->getExprType().value() == ExprType::ShiftOp ||
       expr->getExprType().value() == ExprType::GatherOp ||
       expr->getExprType().value() == ExprType::ViewOp)) {
    return true;
  }
  return false;
}

bool isTVOp(const kir::Expr* expr) {
  const auto& outputs = expr->outputs();
  return outputs.size() >= 1 && outputs[0]->isA<kir::TensorView>();
}

kir::TensorView* getTv(kir::Val* val) {
  if (auto tv = dynamic_cast<kir::TensorView*>(val)) {
    return tv;
  } else if (auto ti = dynamic_cast<kir::TensorIndex*>(val)) {
    return ti->view();
  }
  return nullptr;
}

std::vector<kir::TensorView*> getTvs(const std::vector<kir::Val*>& vals) {
  std::vector<kir::TensorView*> tvs;
  for (auto val : vals) {
    auto tv = ir_utils::getTv(val);
    if (tv) {
      tvs.emplace_back(tv);
    }
  }
  return tvs;
}

kir::TensorView* asTv(kir::Val* val) {
  auto tv = getTv(val);
  TORCH_INTERNAL_ASSERT(tv != nullptr, "Neigher TensorView nor TensorIndex");
  return tv;
}

std::vector<kir::TensorView*> asTvs(const std::vector<kir::Val*> vals) {
  std::vector<kir::TensorView*> tvs;
  for (auto val : vals) {
    auto tv = ir_utils::asTv(val);
    tvs.emplace_back(tv);
  }
  return tvs;
}

// TODO: why do we assume there's a single TV output?
TensorView* getTVOutput(const Expr* expr) {
  for (auto out : expr->outputs()) {
    if (out->getValType().value() == ValType::TensorView) {
      return out->as<TensorView>();
    }
  }
  return nullptr;
}

kir::TensorView* getTVOutput(const kir::Expr* expr) {
  for (auto out : expr->outputs()) {
    if (auto tv = getTv(out)) {
      return tv;
    }
  }
  return nullptr;
}

bool isScalarOp(const Expr* expr) {
  for (auto out : expr->outputs())
    if (!out->isScalar())
      return false;
  return true;
}

Expr* asExpr(Statement* stmt) {
  TORCH_INTERNAL_ASSERT(stmt->isExpr());
  return stmt->as<Expr>();
}

TensorView* asTV(Val* val) {
  TORCH_INTERNAL_ASSERT(isTV(val));
  return val->as<TensorView>();
}

bool hasBlockSync(const Expr* expr, const ThreadPredicateMap& pred_map) {
  if (!isTVOp(expr)) {
    return false;
  }

  auto tv = getTVOutput(expr);

  if ((expr->isA<ReductionOp>() || expr->isA<WelfordOp>()) &&
      (tv->hasBlockReduction() || tv->hasGridReduction())) {
    return true;
  } else if (expr->isA<BroadcastOp>()) {
    const ParallelTypeBitmap pt_map =
        GpuLower::current()->threadPredMap().getParallelBroadcastDomains(tv);
    return pt_map.any();
  }

  return false;
}

bool hasBlockSync(const kir::Expr* expr, const ThreadPredicateMap& pred_map) {
  if (expr->isA<kir::ReductionOp>() || expr->isA<kir::GridReduction>() ||
      expr->isA<kir::GridBroadcast>() || expr->isA<kir::BroadcastOp>() ||
      expr->isA<kir::WelfordOp>() || expr->isA<kir::GridWelford>()) {
    auto fuser_tv = getTVOutput(expr)->fuserTv();
    auto fuser_expr = fuser_tv->definition();
    TORCH_INTERNAL_ASSERT(fuser_expr != nullptr);
    return hasBlockSync(fuser_expr, pred_map);
  }

  return false;
}

// TODO: Remove
kir::Expr* applyReplacements(
    const std::unordered_map<kir::Expr*, kir::Expr*>& expr_replacement_map,
    kir::Expr* expr) {
  auto handle_scope = [&](kir::Scope& scope) {
    for (const auto i : c10::irange(scope.size())) {
      scope[i] = applyReplacements(expr_replacement_map, scope[i]);
    }
  };

  const auto it = expr_replacement_map.find(expr);
  if (it != expr_replacement_map.end()) {
    return it->second;
  } else {
    if (auto for_loop = dynamic_cast<kir::ForLoop*>(expr)) {
      handle_scope(for_loop->body());
    } else if (auto ite = dynamic_cast<kir::IfThenElse*>(expr)) {
      handle_scope(ite->thenBody());
      handle_scope(ite->elseBody());
    }
    return expr;
  }
}

c10::optional<IterDomain*> getMaybeWarpReductionDim(
    const kir::ReductionOp* node) {
  auto kir_tv = ir_utils::getTVOutput(node);
  if (!kir_tv) {
    return c10::nullopt;
  }
  auto fuser_reduction = kir_tv->fuserTv()->definition()->as<ReductionOp>();
  return getMaybeWarpReductionDim(fuser_reduction);
}

c10::optional<IterDomain*> getMaybeWarpReductionDim(const ReductionOp* node) {
  auto fuser_tv_out = node->out()->as<TensorView>();
  auto fuser_tv_in = node->in()->as<TensorView>();

  // only support reducing to registers for now.
  if (fuser_tv_in->getMemoryType() != MemoryType::Local ||
      fuser_tv_out->getMemoryType() != MemoryType::Local) {
    return c10::nullopt;
  }

  IterDomain* reduction_on_xdim = nullptr;
  for (auto id : fuser_tv_out->domain()->domain()) {
    // Currently warp reduction only allows
    //  serial and block.x parallel reductions
    if (id->isReduction() && id->isParallelized()) {
      if (id->getParallelType() == ParallelType::TIDx) {
        reduction_on_xdim = id;
      } else if (id->isThread()) {
        return c10::nullopt;
      }
    }
  }
  if (!reduction_on_xdim) {
    return c10::nullopt;
  }

  if (!reduction_on_xdim->start()->isZeroInt()) {
    return c10::nullopt;
  }

  if (reduction_on_xdim->hasPaddingToMultipleOfWarp()) {
    return c10::optional<IterDomain*>(reduction_on_xdim);
  }

  if (reduction_on_xdim->extent()->isConstScalar()) {
    auto extent_value = reduction_on_xdim->extent()->getInt().value();
    if (extent_value % at::cuda::warp_size() == 0) {
      return c10::optional<IterDomain*>(reduction_on_xdim);
    }
  }

  return c10::nullopt;
}

bool derivedFromRootCAAxes(const TensorView* tv, IterDomain* axis) {
  std::vector<IterDomain*> ca_axes(
      tv->domain()->domain().begin(),
      tv->domain()->domain().begin() + tv->getComputeAtPosition());

  auto ca_root_vals = IterVisitor::getInputsTo(
      std::vector<Val*>(ca_axes.begin(), ca_axes.end()));

  auto root_vals = IterVisitor::getInputsTo({axis});

  return std::any_of(
      root_vals.begin(), root_vals.end(), [&ca_root_vals](auto root) {
        return std::find(ca_root_vals.begin(), ca_root_vals.end(), root) !=
            ca_root_vals.end();
      });
}

std::unordered_map<ParallelType, kir::IterDomain*, TypeHash> getParallelDomains(
    kir::Val* val) {
  kir::TensorView* kir_tv = nullptr;
  if (val->isA<kir::TensorView>()) {
    kir_tv = val->as<kir::TensorView>();
  } else if (val->isA<kir::TensorIndex>()) {
    kir_tv = val->as<kir::TensorIndex>()->view();
  } else {
    TORCH_INTERNAL_ASSERT("Provided val is not TensorIndex or TensorView.");
  }

  std::unordered_map<ParallelType, kir::IterDomain*, TypeHash> parallel_domains;
  for (auto d : kir_tv->domain()->domain()) {
    if (d->isThread()) {
      parallel_domains.insert(std::make_pair(d->parallelType(), d));
    }
  }
  return parallel_domains;
}

} // namespace ir_utils

namespace loop_utils {

BasicAllocInfo getAllocInformation(
    const TensorView* tv,
    const std::vector<kir::ForLoop*>& for_loops,
    const std::unordered_map<IterDomain*, IterDomain*>& id_map,
    bool use_id_map) {
  BasicAllocInfo info;
  auto gpu_lower = GpuLower::current();
  const auto& loop_map = gpu_lower->caLoopMap();

  bool outer_alloc_found = false;

  for (auto fl : for_loops) {
    if (info.alloc_pos == tv->getComputeAtPosition()) {
      break;
    }

    if (tv->axis(info.alloc_pos)->isReduction()) {
      const auto outputs = FusionGuard::getCurFusion()->getTerminatingOutputs();
      TORCH_INTERNAL_ASSERT(
          std::find(outputs.begin(), outputs.end(), tv) != outputs.end(),
          "Invalid computeAt of T",
          tv->name(),
          ". A reducation axis is detected outside computeAt point even though it is not an output tensor.");
      break;
    }

    auto fl_id = fl->iter_domain();

    if (fl_id->parallelType() == ParallelType::Unroll) {
      break;
    }

    // Shared memory must be allocated outside of unswitched
    // domains. See issue #1133.
    if (fl_id->parallelType() == ParallelType::Unswitch &&
        tv->getMemoryType() == MemoryType::Shared) {
      outer_alloc_found = true;
    }

    // Assume global memory is allocated at outer most scope.
    if (tv->getMemoryType() == MemoryType::Global) {
      outer_alloc_found = true;
    }

    auto local_id = tv->axis(info.alloc_pos);

    if (use_id_map) {
      auto id_it = id_map.find(local_id);
      if (id_it != id_map.end()) {
        local_id = id_it->second;
      }
    }
    auto kir_local_id = gpu_lower->lowerValue(local_id)->as<kir::IterDomain>();

    if (loop_map.areMapped(kir_local_id, fl_id)) {
      info.alloc_pos++;
    }

    info.init_for_loop = fl;

    if (!outer_alloc_found) {
      info.alloc_for_loop = fl;
    }
  }

  return info;
}

} // namespace loop_utils

namespace {

class ReplaceExprInput : public kir::OptOutDispatch {
 public:
  using kir::OptOutDispatch::handle;
  static kir::Expr* replace(
      kir::Expr* expr,
      const std::unordered_map<kir::Val*, kir::Val*>& replacement_map) {
    ReplaceExprInput replacer(expr, replacement_map);
    TORCH_INTERNAL_ASSERT(expr != nullptr);
    replacer.handle(expr);
    TORCH_INTERNAL_ASSERT(replacer.replaced_expr_ != nullptr);
    auto ret_expr = replacer.replaced_expr_;

    // Copy predicates if the original expr is predicated
    if (ret_expr != expr) {
      ret_expr->setPredicate(expr->predicate());
      ret_expr->setWritePredicate(expr->writePredicate());
    }
    return ret_expr;
  }

  static std::vector<kir::Expr*> replace(
      const std::vector<kir::Expr*>& scope,
      const std::unordered_map<kir::Val*, kir::Val*>& replacement_map) {
    std::vector<kir::Expr*> ret_expr;
    ret_expr.reserve(scope.size());

    for (auto expr : scope) {
      ret_expr.push_back(replace(expr, replacement_map));
    }

    return ret_expr;
  }

 private:
  ReplaceExprInput(
      kir::Expr* expr,
      const std::unordered_map<kir::Val*, kir::Val*>& replacement_map)
      : gpu_lower_(GpuLower::current()),
        ir_builder_(gpu_lower_->kernel()),
        replacement_map_(replacement_map) {
    replaced_expr_ = expr;
  }

  c10::optional<std::unordered_map<kir::Val*, kir::Val*>>
  getMaybeInputReplacementMap(kir::Expr* expr) {
    bool need_replacement = false;

    std::unordered_map<kir::Val*, kir::Val*> replaced_val;
    for (auto in : expr->inputs()) {
      auto replace_it = replacement_map_.find(in);
      if (replace_it != replacement_map_.end()) {
        need_replacement = true;
        replaced_val[in] = replace_it->second;
      } else {
        replaced_val[in] = in;
      }
    }
    if (need_replacement) {
      return c10::optional<std::unordered_map<kir::Val*, kir::Val*>>(
          replaced_val);
    } else {
      return c10::nullopt;
    }
  }

  // IR visitor interface
  void handle(kir::ForLoop* for_loop) final {
    auto new_for_loop = ir_builder_.create<kir::ForLoop>(for_loop);

    auto replaced_loop_body =
        replace(for_loop->body().exprs(), replacement_map_);

    for (auto new_expr : replaced_loop_body) {
      new_for_loop->body().push_back(new_expr);
    }
    replaced_expr_ = new_for_loop;
  }

  void handle(kir::IfThenElse* ite) final {
    auto new_ite = ir_builder_.create<kir::IfThenElse>(ite->predicate());
    auto replaced_then_body =
        replace(ite->thenBody().exprs(), replacement_map_);
    for (auto new_expr : replaced_then_body) {
      new_ite->thenBody().push_back(new_expr);
    }
    if (ite->hasElse()) {
      auto replaced_else_body =
          replace(ite->elseBody().exprs(), replacement_map_);
      for (auto new_expr : replaced_else_body) {
        new_ite->elseBody().push_back(new_expr);
      }
    }
    replaced_expr_ = new_ite;
  }

  void handle(kir::UnaryOp* node) final {
    auto replaced_inputs = getMaybeInputReplacementMap(node);
    if (replaced_inputs.has_value()) {
      replaced_expr_ = ir_builder_.create<kir::UnaryOp>(
          node->operation(),
          node->out(),
          replaced_inputs.value().at(node->in()));
    }
  }
  void handle(kir::BinaryOp* node) final {
    auto replaced_inputs = getMaybeInputReplacementMap(node);
    if (replaced_inputs.has_value()) {
      replaced_expr_ = ir_builder_.create<kir::BinaryOp>(
          node->operation(),
          node->out(),
          replaced_inputs.value().at(node->lhs()),
          replaced_inputs.value().at(node->rhs()));
    }
  }

  void handle(kir::TernaryOp* node) final {
    auto replaced_inputs = getMaybeInputReplacementMap(node);
    if (replaced_inputs.has_value()) {
      replaced_expr_ = ir_builder_.create<kir::TernaryOp>(
          node->operation(),
          node->out(),
          replaced_inputs.value().at(node->in1()),
          replaced_inputs.value().at(node->in2()),
          replaced_inputs.value().at(node->in3()));
    }
  }

  void handle(kir::ReductionOp* node) final {
    auto replaced_inputs = getMaybeInputReplacementMap(node);
    if (replaced_inputs.has_value()) {
      replaced_expr_ = ir_builder_.create<kir::ReductionOp>(
          node->operation(),
          node->init(),
          node->out(),
          replaced_inputs.value().at(node->in()));
    }
  }

  void handle(kir::BroadcastOp* node) final {
    auto replaced_inputs = getMaybeInputReplacementMap(node);
    if (replaced_inputs.has_value()) {
      replaced_expr_ = ir_builder_.create<kir::BroadcastOp>(
          node->out(), replaced_inputs.value().at(node->in()));
    }
  }

  void handle(kir::WelfordOp* node) final {
    auto replaced_inputs = getMaybeInputReplacementMap(node);
    if (replaced_inputs.has_value()) {
      replaced_expr_ = ir_builder_.create<kir::WelfordOp>(
          node->outAvg(),
          node->outVar(),
          node->outN(),
          node->initAvg(),
          node->initVar(),
          node->initN(),
          replaced_inputs.value().at(node->inAvg()),
          replaced_inputs.value().at(node->inVar()),
          replaced_inputs.value().at(node->inN()));
    }
  }

 private:
  GpuLower* gpu_lower_;
  kir::IrBuilder ir_builder_;
  kir::Expr* replaced_expr_ = nullptr;
  const std::unordered_map<kir::Val*, kir::Val*>& replacement_map_;
};

} // namespace

std::vector<kir::Expr*> replaceInputsInExpr(
    const std::vector<kir::Expr*>& exprs,
    const std::unordered_map<kir::Val*, kir::Val*>& replacement_map) {
  return ReplaceExprInput::replace(exprs, replacement_map);
}

} // namespace cuda
} // namespace fuser
} // namespace jit
} // namespace torch
