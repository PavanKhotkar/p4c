/*
Copyright 2013-present Barefoot Networks, Inc.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/

#include "resolveReferences.h"

#include "frontends/common/parser_options.h"

namespace P4 {

ResolutionContext::ResolutionContext() { anyOrder = P4CContext::get().options().isv1(); }

const std::vector<const IR::IDeclaration *> &ResolutionContext::memoizeDeclarations(
    const IR::INamespace *ns) const {
    std::vector<const IR::IDeclaration *> decls;
    if (const auto *nest = ns->to<IR::INestedNamespace>()) {
        for (const auto *nn : nest->getNestedNamespaces()) {
            auto *nnDecls = nn->getDeclarations();
            decls.insert(decls.end(), nnDecls->begin(), nnDecls->end());
        }
    }

    auto *nsDecls = ns->getDeclarations();
    decls.insert(decls.end(), nsDecls->begin(), nsDecls->end());

    return (namespaceDecls[ns] = std::move(decls));
}

ResolutionContext::NamespaceDeclsByName &ResolutionContext::memoizeDeclsByName(
    const IR::INamespace *ns) const {
    auto &namesToDecls = namespaceDeclNames[ns];
    for (const auto *d : getDeclarations(ns)) namesToDecls[d->getName().name].push_back(d);
    return namesToDecls;
}

std::vector<const IR::IDeclaration *> ResolutionContext::resolve(const IR::ID &name,
                                                                 P4::ResolutionType type) const {
    const Context *ctxt = nullptr;
    while (auto scope = findOrigCtxt<IR::INamespace>(ctxt)) {
        auto rv = lookup(scope, name, type);
        if (!rv.empty()) return rv;
    }
    if (type == P4::ResolutionType::Any) return lookupMatchKind(name);
    return {};
}

std::vector<const IR::IDeclaration *> ResolutionContext::lookup(const IR::INamespace *current,
                                                                const IR::ID &name,
                                                                P4::ResolutionType type) const {
    LOG2("Trying to resolve in " << dbp(current));

    if (const auto *gen = current->to<IR::IGeneralNamespace>()) {
        // FIXME: implement range filtering without enumerator wrappers
        auto *decls = getDeclsByName(gen, name);
        switch (type) {
            case P4::ResolutionType::Any:
                break;
            case P4::ResolutionType::Type: {
                decls = decls->where([](const IR::IDeclaration *d) { return d->is<IR::Type>(); });
                break;
            }
            case P4::ResolutionType::TypeVariable: {
                decls =
                    decls->where([](const IR::IDeclaration *d) { return d->is<IR::Type_Var>(); });
                break;
            }
            default:
                BUG("Unexpected enumeration value %1%", static_cast<int>(type));
        }

        if (!anyOrder && name.srcInfo.isValid()) {
            auto locationFilter = [this, name, type](const IR::IDeclaration *d) {
                if (d->is<IR::Type_Var>() || d->is<IR::ParserState>())
                    // type vars and parser states may be used before their definitions
                    return true;
                Util::SourceInfo nsi = name.srcInfo;
                Util::SourceInfo dsi = d->getNode()->srcInfo;
                bool before = dsi <= nsi;
                LOG3("\tPosition test:" << dsi << "<=" << nsi << "=" << before);

                if (type == ResolutionType::Type) {
                    if (auto *type_decl = findOrigCtxt<IR::Type_Declaration>())
                        if (type_decl->getNode() == d->getNode()) {
                            ::P4::error(ErrorType::ERR_UNSUPPORTED,
                                        "Self-referencing types not supported: '%1%' within '%2%'",
                                        name, d->getNode());
                        }
                } else if (type == ResolutionType::Any) {
                    if (auto *decl_ctxt = findOrigCtxt<IR::Declaration>())
                        if (decl_ctxt->getNode() == d->getNode()) before = false;
                }

                return before;
            };
            decls = decls->where(locationFilter);
        }

        auto vector = decls->toVector();
        if (!vector.empty()) {
            LOG3("Resolved in " << dbp(current->getNode()));
            return vector;
        }
    } else if (const auto *simple = current->to<IR::ISimpleNamespace>()) {
        const auto *decl = simple->getDeclByName(name);
        if (decl) {
            switch (type) {
                case P4::ResolutionType::Any:
                    break;
                case P4::ResolutionType::Type: {
                    if (!decl->is<IR::Type>()) decl = nullptr;
                    break;
                }
                case P4::ResolutionType::TypeVariable: {
                    if (!decl->is<IR::Type_Var>()) decl = nullptr;
                    break;
                }
                default:
                    BUG("Unexpected enumeration value %1%", static_cast<int>(type));
            }
        }
        if (decl) {
            if (!anyOrder && name.srcInfo.isValid() &&
                !current->is<IR::Method>() &&  // method params may be referenced in annotations
                                               // before the method
                !decl->is<IR::Type_Var>() && !decl->is<IR::ParserState>()
                // type vars and parser states may be used before their definitions
            ) {
                Util::SourceInfo nsi = name.srcInfo;
                Util::SourceInfo dsi = decl->getNode()->srcInfo;
                bool before = dsi <= nsi;
                LOG3("\tPosition test:" << dsi << "<=" << nsi << "=" << before);

                if (type == ResolutionType::Any)
                    if (auto *ctxt = findOrigCtxt<IR::Declaration>()) {
                        if (ctxt->getNode() == decl->getNode()) before = false;
                    }

                if (!before) decl = nullptr;
            }
        }
        if (decl) {
            LOG3("Resolved in " << dbp(current->getNode()));
            return {decl};
        }
    } else {
        BUG_CHECK(current->is<IR::INestedNamespace>(), "Unhandled namespace type %s",
                  current->node_type_name());
    }
    if (const auto *nested = current->to<IR::INestedNamespace>()) {
        auto nestedNamespaces = nested->getNestedNamespaces();
        for (const auto *nn : Util::iterator_range(nestedNamespaces).reverse()) {
            auto rv = lookup(nn, name, type);
            if (!rv.empty()) return rv;
        }
    }
    return {};
}

std::vector<const IR::IDeclaration *> ResolutionContext::lookupMatchKind(const IR::ID &name) const {
    LOG2("Resolving " << name << " as match kind");
    if (const auto *global = findOrigCtxt<IR::P4Program>()) {
        for (const auto *obj : global->objects) {
            if (const auto *match_kind = obj->to<IR::Declaration_MatchKind>()) {
                auto rv = lookup(match_kind, name, ResolutionType::Any);
                if (!rv.empty()) return rv;
            }
        }
    }
    return {};
}

const IR::Vector<IR::Argument> *ResolutionContext::methodArguments(cstring name) const {
    const Context *ctxt = getChildContext();
    while (ctxt) {
        const auto *node = ctxt->original;
        const IR::MethodCallExpression *mc = nullptr;
        if (const auto *mcs = node->to<IR::MethodCallStatement>())
            mc = mcs->methodCall;
        else
            mc = node->to<IR::MethodCallExpression>();

        if (mc) {
            if (const auto *mem = mc->method->to<IR::Member>()) {
                if (mem->member == name) return mc->arguments;
            }
            if (const auto *path = mc->method->to<IR::PathExpression>()) {
                if (path->path->name == name) return mc->arguments;
            }
            break;
        }

        if (const auto *decl = node->to<IR::Declaration_Instance>()) {
            if (decl->name == name) return decl->arguments;
            if (const auto *type = decl->type->to<IR::Type_Name>()) {
                if (type->path->name == name) return decl->arguments;
            }
            if (const auto *ts = decl->type->to<IR::Type_Specialized>()) {
                if (ts->baseType->path->name == name) return decl->arguments;
            }
            break;
        }
        if (ctxt->node->is<IR::Expression>() || ctxt->node->is<IR::Type>())
            ctxt = ctxt->parent;
        else
            break;
    }
    LOG4("No arguments found for calling " << name << " in " << ctxt->node);

    return nullptr;
}

const IR::IDeclaration *ResolutionContext::resolveUnique(const IR::ID &name,
                                                         P4::ResolutionType type,
                                                         const IR::INamespace *ns) const {
    LOG2("Resolving " << name << " "
                      << (type == ResolutionType::Type ? "as type" : "as identifier"));

    auto decls = ns ? lookup(ns, name, type) : resolve(name, type);
    LOG3("Lookup resulted in " << decls.size() << " declarations");

    // Check overloaded symbols.
    const IR::Vector<IR::Argument> *arguments;
    if (decls.size() > 1 && (arguments = methodArguments(name))) {
        LOG4("Resolved arguments " << arguments << ". Performing additional overload check");
        decls = Util::enumerate(decls)
                    ->where([arguments](const IR::IDeclaration *d) {
                        auto func = d->to<IR::IFunctional>();
                        if (func == nullptr) return true;
                        return func->callMatches(arguments);
                    })
                    ->toVector();
    }

    if (decls.empty()) {
        ::P4::error(ErrorType::ERR_NOT_FOUND, "%1%: declaration not found", name);
        return nullptr;
    }
    if (decls.size() == 1) {
        LOG2("Lookup result: " << dbp(decls.front()));
        return decls.front();
    }

    ::P4::error(ErrorType::ERR_DUPLICATE, "%1%: multiple matching declarations", name);
    for (const auto *a : decls) ::P4::error(ErrorType::ERR_DUPLICATE, "Candidate: %1%", a);
    return nullptr;
}

const IR::IDeclaration *ResolutionContext::getDeclaration(const IR::Path *path,
                                                          bool notNull) const {
    const IR::IDeclaration *result = nullptr;
    const Context *ctxt = nullptr;
    if (findOrigCtxt<IR::KeyElement>(ctxt) && ctxt->child_index == 1) {
        // looking up a matchType in a key, so need to do a special lookup
        auto decls = lookupMatchKind(path->name);
        if (decls.empty()) {
            ::P4::error(ErrorType::ERR_NOT_FOUND, "%1%: declaration not found", path->name);
        } else if (decls.size() != 1) {
            ::P4::error(ErrorType::ERR_DUPLICATE, "%1%: multiple matching declarations",
                        path->name);
            for (const auto *a : decls) ::P4::error(ErrorType::ERR_DUPLICATE, "Candidate: %1%", a);
        } else {
            result = decls.front();
        }
    } else {
        ResolutionType rtype = ResolutionType::Any;
        if (getParent<IR::Type_Name>() || getOriginal()->is<IR::Type_Name>())
            rtype = ResolutionType::Type;
        const IR::INamespace *ns = nullptr;
        if (path->absolute) ns = findOrigCtxt<IR::P4Program>();

        result = resolveUnique(path->name, rtype, ns);
    }
    if (notNull) BUG_CHECK(result != nullptr, "Cannot find declaration for %1%", path);
    return result;
}

const IR::IDeclaration *ResolutionContext::getDeclaration(const IR::This *pointer,
                                                          bool notNull) const {
    auto result = findOrigCtxt<IR::Declaration_Instance>();
    if (findOrigCtxt<IR::Function>() == nullptr || result == nullptr)
        ::P4::error(ErrorType::ERR_INVALID,
                    "%1% can only be used in the definition of an abstract method", pointer);
    if (notNull) BUG_CHECK(result != nullptr, "Cannot find declaration for %1%", pointer);
    return result;
}

const IR::Type *ResolutionContext::resolveType(const IR::Type *type) const {
    if (auto tname = type->to<IR::Type_Name>())
        return resolvePath(tname->path, true)->to<IR::Type>();
    return type;
}

ResolveReferences::ResolveReferences(ReferenceMap *refMap, bool checkShadow)
    : refMap(refMap), checkShadow(checkShadow) {
    CHECK_NULL(refMap);
    setName("ResolveReferences");
    visitDagOnce = false;
}

const IR::IDeclaration *ResolutionContext::resolvePath(const IR::Path *path, bool isType) const {
    LOG2("Resolving path " << path << " " << (isType ? "as type" : "as identifier"));
    const IR::INamespace *ctxt = nullptr;
    if (path->absolute) ctxt = findOrigCtxt<IR::P4Program>();
    ResolutionType k = isType ? ResolutionType::Type : ResolutionType::Any;
    return resolveUnique(path->name, k, ctxt);
}

const IR::IDeclaration *ResolveReferences::resolvePath(const IR::Path *path, bool isType) const {
    auto decl = ResolutionContext::resolvePath(path, isType);
    if (decl == nullptr) {
        refMap->usedName(path->name.name);
    } else {
        refMap->setDeclaration(path, decl);
    }
    return decl;
}

void ResolveReferences::checkShadowing(const IR::INamespace *ns) const {
    if (!checkShadow) return;
    // FIXME: Likely the map is small, can use flat_map or something similar
    std::unordered_map<cstring, const IR::Node *>
        prev_in_scope;  // check for shadowing within a scope
    auto decls = getDeclarations(ns);
    for (const auto *decl : decls) {
        const IR::Node *node = decl->getNode();
        if (node->is<IR::StructField>()) continue;

        if (node->is<IR::Parameter>() && isInContext<IR::Method>())
            // do not give shadowing warnings for parameters of extern methods
            continue;

        if (prev_in_scope.count(decl->getName()))
            ::P4::warning(ErrorType::WARN_SHADOWING, "'%1%' shadows '%2%'", node,
                          prev_in_scope.at(decl->getName()));
        else if (!node->is<IR::Method>() && !node->is<IR::Function>())
            prev_in_scope[decl->getName()] = node;
        auto prev = resolve(decl->getName(), ResolutionType::Any);
        if (prev.empty()) continue;

        for (const auto *p : prev) {
            const IR::Node *pnode = p->getNode();
            if (pnode == node) continue;
            if ((pnode->is<IR::Method>() || pnode->is<IR::Type_Extern>() ||
                 pnode->is<IR::P4Program>()) &&
                (node->is<IR::Method>() || node->is<IR::Function>() || node->is<IR::P4Control>() ||
                 node->is<IR::P4Parser>() || node->is<IR::Type_Package>()))
                // These can overload each other.
                // Also, the constructor is supposed to have the same name as the class.
                continue;
            if (pnode->is<IR::Attribute>() && node->is<IR::AttribLocal>())
                // attribute locals often match attributes
                continue;

            // parameter shadowing
            if (node->is<IR::Declaration>() && !node->is<IR::Parameter>()) {
                const auto *decl_node = node->to<IR::Declaration>();
                if (const auto *param = pnode->to<IR::Parameter>())
                    if (decl_node->name.name == param->name.name)
                        ::P4::error(ErrorType::WARN_SHADOWING,
                                    "declaration of '%1%' shadows a parameter '%2%'", node, pnode);
            }

            ::P4::warning(ErrorType::WARN_SHADOWING, "'%1%' shadows '%2%'", node, pnode);
        }
    }
}

Visitor::profile_t ResolveReferences::init_apply(const IR::Node *node) {
    anyOrder = refMap->isV1();
    // Check shadowing even if the program map is up-to-date.
    if (!refMap->checkMap(node) || checkShadow) refMap->clear();
    return Inspector::init_apply(node);
}

void ResolveReferences::end_apply(const IR::Node *node) { refMap->updateMap(node); }

// Visitor methods

bool ResolveReferences::preorder(const IR::P4Program *program) {
    if (refMap->checkMap(program)) return false;
    return true;
}

void ResolveReferences::postorder(const IR::P4Program *) { LOG2("Reference map " << refMap); }

bool ResolveReferences::preorder(const IR::This *pointer) {
    auto decl = findContext<IR::Declaration_Instance>();
    if (!isInContext<IR::Function>() || decl == nullptr) {
        ::P4::error(ErrorType::ERR_INVALID,
                    "'%1%' can only be used in the definition of an abstract method", pointer);
        return false;
    }
    refMap->setDeclaration(pointer, decl);
    return true;
}

bool ResolveReferences::preorder(const IR::KeyElement *ke) {
    visit(ke->annotations, "annotations");
    visit(ke->expression, "expression");
    auto decls = lookupMatchKind(ke->matchType->path->name);
    if (decls.empty()) {
        ::P4::error(ErrorType::ERR_NOT_FOUND, "%1%: declaration not found",
                    ke->matchType->path->name);
        refMap->usedName(ke->matchType->path->name.name);
    } else if (decls.size() != 1) {
        ::P4::error(ErrorType::ERR_DUPLICATE, "%1%: multiple matching declarations",
                    ke->matchType->path->name);
        for (const auto *a : decls) ::P4::error(ErrorType::ERR_DUPLICATE, "Candidate: %1%", a);
    } else {
        refMap->setDeclaration(ke->matchType->path, decls.front());
    }
    return false;
}

bool ResolveReferences::preorder(const IR::PathExpression *path) {
    resolvePath(path->path, false);
    return true;
}

bool ResolveReferences::preorder(const IR::Type_Name *type) {
    resolvePath(type->path, true);
    return true;
}

bool ResolveReferences::preorder(const IR::P4Control *c) {
    refMap->usedName(c->name.name);
    checkShadowing(c);
    return true;
}

bool ResolveReferences::preorder(const IR::P4Parser *p) {
    refMap->usedName(p->name.name);
    checkShadowing(p);
    return true;
}

bool ResolveReferences::preorder(const IR::Function *function) {
    refMap->usedName(function->name.name);
    checkShadowing(function);
    return true;
}

bool ResolveReferences::preorder(const IR::P4Table *t) {
    refMap->usedName(t->name.name);
    return true;
}

bool ResolveReferences::preorder(const IR::TableProperties *p) {
    checkShadowing(p);
    return true;
}

bool ResolveReferences::preorder(const IR::P4Action *c) {
    refMap->usedName(c->name.name);
    checkShadowing(c);
    return true;
}

bool ResolveReferences::preorder(const IR::Type_Method *t) {
    checkShadowing(t);
    return true;
}

bool ResolveReferences::preorder(const IR::Type_Extern *t) {
    refMap->usedName(t->name.name);
    checkShadowing(t);
    return true;
}

bool ResolveReferences::preorder(const IR::ParserState *s) {
    refMap->usedName(s->name.name);
    checkShadowing(s);
    return true;
}

bool ResolveReferences::preorder(const IR::Type_ArchBlock *t) {
    if (!t->is<IR::Type_Package>()) {
        // don't check shadowing in packages as they have no body
        checkShadowing(t);
    }
    return true;
}

void ResolveReferences::postorder(const IR::Type_ArchBlock *t) { refMap->usedName(t->name.name); }

bool ResolveReferences::preorder(const IR::Type_StructLike *t) {
    refMap->usedName(t->name.name);
    checkShadowing(t);
    return true;
}

bool ResolveReferences::preorder(const IR::BlockStatement *b) {
    checkShadowing(b);
    return true;
}

bool ResolveReferences::preorder(const IR::Declaration_Instance *decl) {
    refMap->usedName(decl->name.name);
    return true;
}

#undef PROCESS_NAMESPACE

}  // namespace P4
