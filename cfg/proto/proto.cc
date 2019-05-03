// have to be included first as they violate our poisons
#include "cfg/proto/proto.h"

#include "core/proto/proto.h"

using namespace std;

namespace sorbet::cfg {

com::stripe::rubytyper::TypedVariable Proto::toProto(const core::GlobalState &gs, const VariableUseSite &vus,
                                                     const core::Loc *loc) {
    com::stripe::rubytyper::TypedVariable proto;
    proto.set_name(vus.variable.toString(gs));
    if (vus.type) {
        *proto.mutable_type() = core::Proto::toProto(gs, vus.type);
    }
    if (loc) {
        *proto.mutable_location() = core::Proto::toProto(gs, *loc);
    }
    return proto;
}

com::stripe::rubytyper::Instruction Proto::toProto(const core::GlobalState &gs, const Instruction *instr) {
    com::stripe::rubytyper::Instruction proto;
    typecase(
        instr,
        [&](const Ident *i) {
            proto.set_kind(com::stripe::rubytyper::Instruction::IDENT);
            proto.set_ident(i->what.toString(gs));
        },
        [&](const Alias *i) {
            proto.set_kind(com::stripe::rubytyper::Instruction::ALIAS);
            *proto.mutable_alias() = core::Proto::toProtoNoChildren(gs, i->what);
        },
        [&](const Send *i) {
            proto.set_kind(com::stripe::rubytyper::Instruction::SEND);
            *proto.mutable_send()->mutable_receiver() = toProto(gs, i->recv, &i->receiverLoc);
            *proto.mutable_send()->mutable_method() = core::Proto::toProto(gs, i->fun);
            if (i->link) {
                *proto.mutable_send()->mutable_block() = com::stripe::rubytyper::Instruction::Block();
            }
            for (size_t j = 0; j < i->args.size(); ++j) {
                *proto.mutable_send()->add_arguments() = toProto(gs, i->args[j], &i->argLocs[j]);
            }
        },
        [&](const Return *i) {
            proto.set_kind(com::stripe::rubytyper::Instruction::RETURN);
            *proto.mutable_return_() = toProto(gs, i->what);
        },
        [&](const Literal *i) {
            proto.set_kind(com::stripe::rubytyper::Instruction::LITERAL);
            *proto.mutable_literal() = core::Proto::toProto(gs, i->value);
        },
        [&](const Unanalyzable *i) { proto.set_kind(com::stripe::rubytyper::Instruction::UNANALYZABLE); },
        [&](const LoadArg *i) {
            proto.set_kind(com::stripe::rubytyper::Instruction::LOAD_ARG);
            *proto.mutable_load_arg()->mutable_argument() = core::Proto::toProtoNoChildren(gs, i->arg);
            proto.mutable_load_arg()->set_argument_name(i->arg.data(gs)->argumentName(gs));
        },
        [&](const Cast *i) {
            proto.set_kind(com::stripe::rubytyper::Instruction::CAST);
            *proto.mutable_cast()->mutable_value() = toProto(gs, i->value);
            *proto.mutable_cast()->mutable_type() = core::Proto::toProto(gs, i->type);
        },
        // TODO later: add more types
        [&](const Instruction *i) { proto.set_kind(com::stripe::rubytyper::Instruction::UNKNOWN); });
    return proto;
}

com::stripe::rubytyper::Binding Proto::toProto(const core::GlobalState &gs, const Binding &bnd) {
    com::stripe::rubytyper::Binding proto;
    *proto.mutable_bind() = toProto(gs, bnd.bind, &bnd.loc);
    *proto.mutable_instruction() = toProto(gs, bnd.value.get());
    return proto;
}

com::stripe::rubytyper::Block::BlockExit Proto::toProto(const core::GlobalState &gs, const BlockExit &ex) {
    com::stripe::rubytyper::Block::BlockExit proto;
    if (ex.cond.variable.exists()) {
        *proto.mutable_cond() = toProto(gs, ex.cond);
    }
    if (ex.thenb) {
        proto.set_then_block(ex.thenb->id);
    }
    if (ex.elseb) {
        proto.set_else_block(ex.elseb->id);
    }
    *proto.mutable_location() = core::Proto::toProto(gs, ex.loc);
    return proto;
}

com::stripe::rubytyper::Block Proto::toProto(const core::GlobalState &gs, const BasicBlock &bb) {
    com::stripe::rubytyper::Block proto;
    proto.set_id(bb.id);
    for (auto const &bnd : bb.exprs) {
        *proto.add_bindings() = toProto(gs, bnd);
    }
    *proto.mutable_exit() = toProto(gs, bb.bexit);
    return proto;
}

com::stripe::rubytyper::CFG::Argument Proto::argumentToProto(const core::GlobalState &gs, core::SymbolRef sym) {
    com::stripe::rubytyper::CFG::Argument proto;

    core::SymbolData s = sym.data(gs);
    proto.set_name(s->argumentName(gs));
    if (s->resultType) {
        *proto.mutable_type() = core::Proto::toProto(gs, s->resultType);
    }
    return proto;
}

com::stripe::rubytyper::CFG Proto::toProto(const core::GlobalState &gs, const CFG &cfg) {
    com::stripe::rubytyper::CFG proto;
    core::SymbolData sym = cfg.symbol.data(gs);

    *proto.mutable_symbol() = core::Proto::toProto(gs, cfg.symbol);
    *proto.mutable_location() = core::Proto::toProto(gs, sym->loc());

    if (sym->resultType) {
        *proto.mutable_returns() = core::Proto::toProto(gs, sym->resultType);
    }

    for (auto arg : sym->arguments()) {
        *proto.add_arguments() = argumentToProto(gs, arg);
    }

    for (auto const &block : cfg.basicBlocks) {
        *proto.add_blocks() = toProto(gs, *block);
    }
    return proto;
}

} // namespace sorbet::cfg
