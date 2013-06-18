/*
   +----------------------------------------------------------------------+
   | HipHop for PHP                                                       |
   +----------------------------------------------------------------------+
   | Copyright (c) 2010-2013 Facebook, Inc. (http://www.facebook.com)     |
   +----------------------------------------------------------------------+
   | This source file is subject to version 3.01 of the PHP license,      |
   | that is bundled with this package in the file LICENSE, and is        |
   | available through the world-wide-web at the following url:           |
   | http://www.php.net/license/3_01.txt                                  |
   | If you did not receive a copy of the PHP license and are unable to   |
   | obtain it through the world-wide-web, please send a note to          |
   | license@php.net so we can mail you a copy immediately.               |
   +----------------------------------------------------------------------+
*/
#include "hphp/runtime/vm/jit/region_selection.h"

#include <algorithm>
#include <boost/range/adaptors.hpp>

#include "folly/Memory.h"
#include "folly/Conv.h"

#include "hphp/util/assertions.h"
#include "hphp/util/map_walker.h"
#include "hphp/runtime/base/runtime_option.h"
#include "hphp/runtime/vm/jit/translator.h"

namespace HPHP { namespace JIT {

TRACE_SET_MOD(region);

//////////////////////////////////////////////////////////////////////

extern RegionDescPtr regionMethod(const RegionContext&);
extern RegionDescPtr regionOneBC(const RegionContext&);

//////////////////////////////////////////////////////////////////////

namespace {

enum class RegionMode {
  None,
  OneBC,
  Method,
  Tracelet,
};

RegionMode regionMode() {
  auto& s = RuntimeOption::EvalJitRegionSelector;
  if (s == "")       return RegionMode::None;
  if (s == "onebc")  return RegionMode::OneBC;
  if (s == "method") return RegionMode::Method;
  if (s == "tracelet") return RegionMode::Tracelet;
  FTRACE(1, "unknown region mode {}: using none\n", s);
  if (debug) abort();
  return RegionMode::None;
}

}

//////////////////////////////////////////////////////////////////////

void RegionDesc::Block::addPredicted(SrcKey sk, TypePred pred) {
  assert(pred.type.subtypeOf(Type::Gen | Type::Cls));
  m_typePreds.insert(std::make_pair(sk, pred));
  checkInvariants();
}

void RegionDesc::Block::setParamByRef(SrcKey sk, ParamByRef byRef) {
  assert(m_byRefs.find(sk) == m_byRefs.end());
  m_byRefs.insert(std::make_pair(sk, byRef));
  checkInvariants();
}

void RegionDesc::Block::addReffinessPred(SrcKey sk, const ReffinessPred& pred) {
  m_refPreds.insert(std::make_pair(sk, pred));
  checkInvariants();
}

/*
 * Check invariants on a RegionDesc::Block.
 *
 * 1. Single entry, single exit (aside from exceptions).  I.e. no
 *    non-fallthrough instructions mid-block and no control flow (not
 *    counting calls as control flow).
 *
 * 2. Each SrcKey in m_typePreds, m_byRefs, and m_refPreds is within the bounds
 *    of the block.
 *
 * 3. Each local id referred to in the type prediction list is valid.
 *
 * 4. (Unchecked) each stack offset in the type prediction list is
 *    valid.
 */
void RegionDesc::Block::checkInvariants() const {
  if (!debug || length() == 0) return;

  smart::set<SrcKey> keysInRange;
  auto firstKey = [&] { return *keysInRange.begin(); };
  auto lastKey = [&] {
    assert(!keysInRange.empty());
    return *--keysInRange.end();
  };
  keysInRange.insert(start());
  for (int i = 1; i < length(); ++i) {
    if (i != length() - 1) {
      auto const pc = unit()->at(lastKey().offset());
      if (instrFlags(toOp(*pc)) & TF) {
        FTRACE(1, "Bad block: {}\n", show(*this));
        assert(!"Block may not contain non-fallthrough instruction unless "
                "they are last");
      }
      if (instrIsNonCallControlFlow(toOp(*pc))) {
        FTRACE(1, "Bad block: {}\n", show(*this));
        assert(!"Block may not contain control flow instructions unless "
                "they are last");
      }
    }
    keysInRange.insert(lastKey().advanced(unit()));
  }
  assert(keysInRange.size() == length());

  auto rangeCheck = [&](const char* type, SrcKey sk) {
    if (!keysInRange.count(sk)) {
      std::cerr << folly::format("{} at {} outside range [{}, {}]\n",
                                type, show(sk),
                                 show(firstKey()), show(lastKey()));
      assert(!"Region::Block contained out-of-range metadata");
    }
  };
  for (auto& tpred : m_typePreds) {
    rangeCheck("type prediction", tpred.first);
    auto& loc = tpred.second.location;
    switch (loc.tag()) {
    case Location::Tag::Local: assert(loc.localId() < m_func->numLocals());
                               break;
    case Location::Tag::Stack: // Unchecked
                               break;
    }
  }

  for (auto& byRef : m_byRefs) {
    rangeCheck("parameter reference flag", byRef.first);
  }
  for (auto& refPred : m_refPreds) {
    rangeCheck("reffiness prediction", refPred.first);
  }
}

//////////////////////////////////////////////////////////////////////

namespace {
RegionDescPtr createRegion(const Transl::Tracelet& tlet) {
  typedef Transl::NormalizedInstruction NI;
  typedef RegionDesc::Block Block;

  auto region = smart::make_unique<RegionDesc>();
  SrcKey sk(tlet.m_sk);
  assert(sk == tlet.m_instrStream.first->source);
  auto unit = tlet.m_instrStream.first->unit();

  Block* curBlock;
  auto newBlock = [&] {
    region->blocks.push_back(
      smart::make_unique<Block>(tlet.m_func, sk.offset(), 0));
    curBlock = region->blocks.back().get();
  };
  newBlock();

  for (auto ni = tlet.m_instrStream.first; ni; ni = ni->next) {
    assert(sk == ni->source);
    assert(ni->unit() == unit);

    curBlock->addInstruction();
    if (!ni->noOp && isFPassStar(ni->op())) {
      curBlock->setParamByRef(
        sk, ni->preppedByRef ? RegionDesc::ParamByRef::Yes
                             : RegionDesc::ParamByRef::No);
    }
    if (ni->op() == OpJmp && ni->next) {
      // A Jmp that isn't the final instruction in a Tracelet means we traced
      // through a forward jump in analyze. Update sk to point to the next NI
      // in the stream.
      auto dest = ni->offset() + ni->imm[0].u_BA;
      assert(dest > sk.offset()); // We only trace for forward Jmps for now.
      sk.setOffset(dest);

      // The Jmp terminates this block.
      newBlock();
    } else {
      sk.advance(unit);
    }
  }

  auto& frontBlock = *region->blocks.front();

  // Add tracelet guards as predictions on the first instruction. Predictions
  // and known types from static analysis will be applied by
  // Translator::translateRegion.
  for (auto const& dep : tlet.m_dependencies) {
    if (dep.second->rtt.isVagueValue() ||
        dep.second->location.isThis()) continue;

    typedef RegionDesc R;
    auto addPred = [&](const R::Location& loc) {
      auto type = Type::fromRuntimeType(dep.second->rtt);
      frontBlock.addPredicted(tlet.m_sk, {loc, type});
    };

    switch (dep.first.space) {
      case Transl::Location::Stack:
        addPred(R::Location::Stack{uint32_t(-dep.first.offset - 1)});
        break;

      case Transl::Location::Local:
        addPred(R::Location::Local{uint32_t(dep.first.offset)});
        break;

      default: not_reached();
    }
  }

  // Add reffiness dependencies as predictions on the first instruction.
  for (auto const& dep : tlet.m_refDeps.m_arMap) {
    RegionDesc::ReffinessPred pred{dep.second.m_mask,
                                   dep.second.m_vals,
                                   dep.first};
    frontBlock.addReffinessPred(tlet.m_sk, pred);
  }

  FTRACE(2, "Converted Tracelet:\n{}\nInto RegionDesc:\n{}\n",
         tlet.toString(), show(*region));
  return region;
}
}

RegionDescPtr selectRegion(const RegionContext& context,
                           const Transl::Tracelet* t) {
  auto const mode = regionMode();

  FTRACE(1,
    "Select region: {}@{} mode={} context:\n{}{}",
    context.func->fullName()->data(),
    context.offset,
    static_cast<int>(mode),
    [&]{
      std::string ret;
      for (auto& t : context.liveTypes) {
        folly::toAppend(" ", show(t), "\n", &ret);
      }
      return ret;
    }(),
    [&]{
      std::string ret;
      for (auto& ar : context.preLiveARs) {
        folly::toAppend(" ", show(ar), "\n", &ret);
      }
      return ret;
    }()
  );

  auto region = [&]{
    try {
      switch (mode) {
      case RegionMode::None:   return RegionDescPtr{nullptr};
      case RegionMode::OneBC:  return regionOneBC(context);
      case RegionMode::Method: return regionMethod(context);
      case RegionMode::Tracelet: always_assert(t); return createRegion(*t);
      }
      not_reached();
    } catch (const std::exception& e) {
      FTRACE(1, "region selector threw: {}\n", e.what());
      return RegionDescPtr{nullptr};
    }
  }();

  if (region) {
    FTRACE(3, "{}", show(*region));
  } else {
    FTRACE(1, "no region selectable; using tracelet compiler\n");
  }

  return region;
}

//////////////////////////////////////////////////////////////////////

std::string show(RegionDesc::Location l) {
  switch (l.tag()) {
  case RegionDesc::Location::Tag::Local:
    return folly::format("Local{{{}}}", l.localId()).str();
  case RegionDesc::Location::Tag::Stack:
    return folly::format("Stack{{{}}}", l.stackOffset()).str();
  }
  not_reached();
}

std::string show(RegionDesc::TypePred ta) {
  return folly::format(
    "{} :: {}",
    show(ta.location),
    ta.type.toString()
  ).str();
}

std::string show(const RegionDesc::ReffinessPred& pred) {
  std::ostringstream out;
  out << "offset: " << pred.arSpOffset << " mask: ";
  for (auto const bit : pred.mask) out << (bit ? '1' : '0');
  out << " vals: ";
  for (auto const bit : pred.vals) out << (bit ? '1' : '0');
  return out.str();
}

std::string show(RegionDesc::ParamByRef byRef) {
  switch (byRef) {
    case RegionDesc::ParamByRef::Yes: return "by value";
    case RegionDesc::ParamByRef::No:  return "by reference";
  }
  not_reached();
}

std::string show(RegionContext::LiveType ta) {
  return folly::format(
    "{} :: {}",
    show(ta.location),
    ta.type.toString()
  ).str();
}

std::string show(RegionContext::PreLiveAR ar) {
  return folly::format(
    "AR@{}: {} ({})",
    ar.stackOff,
    ar.func->fullName()->data(),
    ar.objOrCls.toString()
  ).str();
}

std::string show(const RegionDesc::Block& b) {
  std::string ret{"Block "};
  folly::toAppend(
    b.func()->fullName()->data(), '@', b.start().offset(),
    " length ", b.length(), '\n',
    &ret
  );

  auto typePreds = makeMapWalker(b.typePreds());
  auto byRefs    = makeMapWalker(b.paramByRefs());
  auto refPreds  = makeMapWalker(b.reffinessPreds());

  auto skIter = b.start();
  for (int i = 0; i < b.length(); ++i) {
    while (typePreds.hasNext(skIter)) {
      folly::toAppend("  predict: ", show(typePreds.next()), "\n", &ret);
    }
    while (refPreds.hasNext(skIter)) {
      folly::toAppend("  predict reffiness: ", show(refPreds.next()), "\n",
                      &ret);
    }
    std::string byRef;
    if (byRefs.hasNext(skIter)) {
      byRef = folly::format(" (passed {})", show(byRefs.next())).str();
    }
    folly::toAppend(
      "    ",
      skIter.offset(),
      "  ",
      instrToString((Op*)b.unit()->at(skIter.offset()), b.unit()),
      byRef,
      "\n",
      &ret
    );
    skIter.advance(b.unit());
  }
  return ret;
}

std::string show(const RegionDesc& region) {
  return folly::format(
    "Region ({} blocks):\n{}",
    region.blocks.size(),
    [&]{
      std::string ret;
      for (auto& b : region.blocks) {
        folly::toAppend(show(*b), &ret);
      }
      return ret;
    }()
  ).str();
}

//////////////////////////////////////////////////////////////////////

}}
