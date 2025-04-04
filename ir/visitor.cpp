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

#include "visitor.h"

#include <config.h>
#include <stdlib.h>
#include <time.h>

#include "absl/container/flat_hash_map.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "ir/ir-generated.h"
#include "lib/hash.h"

#if HAVE_CXXABI_H
#include <cxxabi.h>
#endif

#if HAVE_LIBGC
#include <gc/gc.h>
#endif

#include "dbprint.h"
#include "ir/id.h"
#include "ir/ir.h"
#include "ir/vector.h"
#include "lib/algorithm.h"
#include "lib/error_catalog.h"
#include "lib/indent.h"
#include "lib/log.h"
#include "lib/map.h"

namespace P4 {

enum class VisitStatus : unsigned { New, Revisit, Busy, Done };

/** @class Visitor::ChangeTracker
 *  @brief Assists visitors in traversing the IR.

 *  A ChangeTracker object assists visitors traversing the IR by tracking each
 *  node.  The `start` method begins tracking, and `finish` ends it.  The
 *  `done` method determines whether the node has been visited, and `result`
 *  returns the new IR if it changed.
 */
class Visitor::ChangeTracker {
    struct visit_info_t {
        // FIXME: We should be able to put these two bools into low 2 bits of
        // `result` saving 8 bytes per record
        bool visit_in_progress;
        bool visitOnce;
        const IR::Node *result;
    };
    using visited_t = absl::flat_hash_map<const IR::Node *, visit_info_t, Util::Hash>;
    bool forceClone;
    visited_t visited;

 public:
    explicit ChangeTracker(bool forceClone)
        : forceClone(forceClone),
          visited(16) {}  // Pre-allocate 16 slots as usually these maps are small, but we do create
                          // lots of them. This saves quite some time for rehashes

    /** Begin tracking @n during a visiting pass.  Use `finish(@n)` to mark @n as
     * visited once the pass completes.
     *
     * @return Status of the node @n wrt visit: whether node is currently being
     * visited (`VisitStatus::Busy`), or if node was already visited
     * (`VisitStatus::Done`), node was never seen before (`VisitStatus::New`) or
     * seen, but should be revisited (`VisitStatus::Revisit`).
     */
    [[nodiscard]] VisitStatus try_start(const IR::Node *n, bool defaultVisitOnce) {
        // Initialization
        auto [it, inserted] = visited.emplace(n, visit_info_t{true, defaultVisitOnce, n});

        if (!inserted) {  // We already seen this node, determine its status
            if (it->second.visit_in_progress) return VisitStatus::Busy;
            if (it->second.visitOnce) return VisitStatus::Done;
            it->second.visit_in_progress = true;
            return VisitStatus::Revisit;
        }

        return VisitStatus::New;
    }

    /** Mark the process of visiting @orig as finished, with @final being the
     * final state of the node, or nullptr if the node was removed from the
     * tree.  `done(@orig)` will return true, and `result(@orig)` will return
     * the resulting node, if any.
     *
     * If @final is a new node, that node is marked as finished as well, as if
     * `start(@final); finish(@final);` were invoked.
     *
     * @return true if the node has changed or been removed or coalesced.
     *
     * @exception Util::CompilerBug This method fails if `start(@orig)` has not
     * previously been invoked.
     */
    bool finish(const IR::Node *orig, const IR::Node *final) {
        auto it = visited.find(orig);
        if (it == visited.end()) BUG("visitor state tracker corrupted");

        visit_info_t *orig_visit_info = &(it->second);
        orig_visit_info->visit_in_progress = false;
        if (!final) {
            orig_visit_info->result = final;
            return true;
        } else if (forceClone || (final != orig && *final != *orig)) {
            orig_visit_info->result = final;
            visited.emplace(final, visit_info_t{false, orig_visit_info->visitOnce, final});
            return true;
        } else if (visited.count(final)) {
            // coalescing with some previously visited node, so we don't want to undo
            // the coalesce
            orig_visit_info->result = final;
            return true;
        } else {
            // FIXME -- not safe if the visitor resurrects the node (which it shouldn't)
            // if (final && final->id == IR::Node::currentId - 1)
            //     --IR::Node::currentId;
            return false;
        }
    }

    /** Return a visitOnce flag for node @n */
    [[nodiscard]] bool shouldVisitOnce(const IR::Node *n) const {
        auto it = visited.find(n);
        if (it == visited.end()) BUG("visitor state tracker corrupted");
        return it->second.visitOnce;
    }

    /** Forget nodes that have already been visited, allowing them to be visited
     * again. */
    void revisit_visited() {
        for (auto it = visited.begin(); it != visited.end();) {
            if (!it->second.visit_in_progress)
                // `visited` is abseil map, therefore erase does not return iterator, use
                // post-increment
                visited.erase(it++);
            else
                ++it;
        }
    }

    /** Determine whether @n is currently being visited and the visitor has not finished
     * That is, `start(@n)` has been invoked, and `finish(@n)` has not,
     *
     * @return true if @n is being visited and has not finished
     */
    [[nodiscard]] bool busy(const IR::Node *n) const {
        auto it = visited.find(n);
        return it != visited.end() && it->second.visit_in_progress;
    }

    /** Determine whether @n has been visited and the visitor has finished
     *  and we don't want to visit @n again the next time we see it.
     * That is, `start(@n)` has been invoked, followed by `finish(@n)`,
     * and the visitOnce field is true.
     *
     * @return true if @n has been visited and the visitor is finished and visitOnce is true
     */
    [[nodiscard]] bool done(const IR::Node *n) const {
        auto it = visited.find(n);
        return it != visited.end() && !it->second.visit_in_progress && it->second.visitOnce;
    }

    /** Produce the result of visiting @n.
     *
     * @return The result of visiting @n, or the intermediate result of
     * visiting @n if `start(@n)` has been invoked but not `finish(@n)`, or @n
     * if `start(@n)` has not been invoked.
     */
    const IR::Node *result(const IR::Node *n) const {
        auto it = visited.find(n);
        if (it == visited.end()) return n;
        return it->second.result;
    }

    /** Produce the final result of visiting @n.
     *
     * @return The ultimate result of visiting @n, or `nullptr` if `finish(@n)` has not
     * been invoked.
     */
    const IR::Node *finalResult(const IR::Node *n) const {
        auto it = visited.find(n);
        bool done = it != visited.end() && !it->second.visit_in_progress && it->second.visitOnce;
        return done ? it->second.result : nullptr;
    }

    void visitOnce(const IR::Node *n) {
        auto it = visited.find(n);
        if (it == visited.end()) BUG("visitor state tracker corrupted");
        it->second.visitOnce = true;
    }

    void visitAgain(const IR::Node *n) {
        auto it = visited.find(n);
        if (it == visited.end()) BUG("visitor state tracker corrupted");
        it->second.visitOnce = false;
    }
};

/** @class Visitor::Tracker
 *  @brief Assists visitors in traversing the IR.

 *  A Tracker object assists visitors traversing the IR by tracking each
 *  node.  The `start` method begins tracking, and `finish` ends it.  The
 *  `done` method determines whether the node has been visited.
 */
class Visitor::Tracker {
    // FIXME: We can squeeze these into low 2 bits of key, eliminating the value entirely
    struct info_t {
        bool done, visitOnce;
    };
    using visited_t = absl::flat_hash_map<const IR::Node *, info_t, Util::Hash>;
    visited_t visited;

 public:
    Tracker()
        : visited(16) {}  // Pre-allocate 16 slots as usually these maps are small, but we do create
                          // lots of them. This saves quite some time for rehashes

    /** Forget nodes that have already been visited, allowing them to be visited
     * again. */
    void revisit_visited() {
        for (auto it = visited.begin(); it != visited.end();) {
            if (it->second.done)
                // `visited` is abseil map, therefore erase does not return iterator, use
                // post-increment
                visited.erase(it++);
            else
                ++it;
        }
    }

    /** Begin tracking @n during a visiting pass.  Use `finish(@n)` to mark @n as
     * visited once the pass completes.
     *
     * @return Status of the node @n wrt visit: whether node is currently being
     * visited (`VisitStatus::Busy`), or if node was already visited
     * (`VisitStatus::Done`), node was never seen before (`VisitStatus::New`) or
     * seen, but should be revisited (`VisitStatus::Revisit`).

     */
    [[nodiscard]] VisitStatus try_start(const IR::Node *n, bool defaultVisitOnce) {
        // Initialization
        auto [it, inserted] = visited.emplace(n, info_t{false, defaultVisitOnce});

        if (!inserted) {  // We already seen this node, determine its status
            if (!it->second.done) return VisitStatus::Busy;
            if (it->second.visitOnce) return VisitStatus::Done;
            it->second.done = false;
            return VisitStatus::Revisit;
        }

        return VisitStatus::New;
    }

    /** Mark the process of visiting @n as finished, with @final being the
     * final state of the node, or nullptr if the node was removed from the
     * tree.  `done(@n)` will return true, and `result(@n)` will return
     * the resulting node, if any.
     *
     * If @final is a new node, that node is marked as finished as well, as if
     * `start(@final); finish(@final);` were invoked.
     *
     * @exception Util::CompilerBug This method fails if `start(@n)` has not
     * previously been invoked.
     */
    void finish(const IR::Node *n) {
        auto it = visited.find(n);
        if (it == visited.end()) BUG("visitor state tracker corrupted");

        it->second.done = true;
    }

    /** Determine whether @n is currently being visited and the visitor has not finished
     * That is, `start(@n)` has been invoked, and `finish(@n)` has not,
     *
     * @return true if @n is being visited and has not finished
     */
    [[nodiscard]] bool busy(const IR::Node *n) const {
        auto it = visited.find(n);
        return it != visited.end() && !it->second.done;
    }

    /** Determine whether @n has been visited and the visitor has finished
     *  and we don't want to visit @n again the next time we see it.
     * That is, `start(@n)` has been invoked, followed by `finish(@n)`,
     * and the visitOnce field is true.
     *
     * @return true if @n has been visited and the visitor is finished and visitOnce is true
     */
    [[nodiscard]] bool done(const IR::Node *n) const {
        auto it = visited.find(n);
        return it != visited.end() && it->second.done && it->second.visitOnce;
    }

    /** Return a visitOnce flag for node @n */
    bool shouldVisitOnce(const IR::Node *n) const {
        auto it = visited.find(n);
        if (it == visited.end()) BUG("visitor state tracker corrupted");
        return it->second.visitOnce;
    }

    void visitOnce(const IR::Node *n) {
        auto it = visited.find(n);
        if (it == visited.end()) BUG("visitor state tracker corrupted");
        it->second.visitOnce = true;
    }

    void visitAgain(const IR::Node *n) {
        auto it = visited.find(n);
        if (it == visited.end()) BUG("visitor state tracker corrupted");
        it->second.visitOnce = false;
    }
};

// static
bool Visitor::warning_enabled(const Visitor *visitor, int warning_kind) {
    auto errorString = ErrorCatalog::getCatalog().getName(warning_kind);
    while (visitor != nullptr) {
        auto crt = visitor->ctxt;
        while (crt != nullptr) {
            if (const auto *annotated = crt->node->to<IR::IAnnotated>()) {
                for (const auto *a : annotated->getAnnotations()) {
                    if (a->name != IR::Annotation::noWarnAnnotation) continue;

                    auto arg = a->getSingleString();
                    if (arg == errorString) return false;
                }
            }
            crt = crt->parent;
        }
        visitor = visitor->called_by;
    }
    return true;
}

Visitor::profile_t Visitor::init_apply(const IR::Node *root) {
    ctxt = nullptr;
    if (joinFlows) init_join_flows(root);
    return profile_t(*this);
}
Visitor::profile_t Visitor::init_apply(const IR::Node *root, const Context *parent_ctxt) {
    auto rv = init_apply(root);
    ctxt = parent_ctxt;
    return rv;
}
Visitor::profile_t Modifier::init_apply(const IR::Node *root) {
    auto rv = Visitor::init_apply(root);
    visited = std::make_shared<ChangeTracker>(forceClone);
    return rv;
}
Visitor::profile_t Inspector::init_apply(const IR::Node *root) {
    auto rv = Visitor::init_apply(root);
    visited = std::make_shared<Tracker>();
    return rv;
}
Visitor::profile_t Transform::init_apply(const IR::Node *root) {
    auto rv = Visitor::init_apply(root);
    visited = std::make_shared<ChangeTracker>(forceClone);
    return rv;
}
void Visitor::end_apply() {}
void Visitor::end_apply(const IR::Node *) {}

static indent_t profile_indent;
static absl::Time first_start = absl::InfinitePast();

Visitor::profile_t::profile_t(Visitor &v_) : v(v_) {
    start = absl::Now();
    LOG3(profile_indent << v.name() << " statrting at +"
                        << (first_start != absl::InfinitePast()
                                ? start - first_start
                                : (first_start = start, start - first_start)));
    ++profile_indent;
}
Visitor::profile_t::profile_t(profile_t &&a) : v(a.v), start(a.start) {
    a.start = absl::InfinitePast();
}
Visitor::profile_t::~profile_t() {
    if (start != absl::InfinitePast()) {
        v.end_apply();
        --profile_indent;
        LOG1(profile_indent << v.name() << ' ' << (absl::Now() - start));
    }
}

void Inspector::visitOnce() const { visited->visitOnce(getOriginal()); }
void Modifier::visitOnce() const { visited->visitOnce(getOriginal()); }
void Transform::visitOnce() const { visited->visitOnce(getOriginal()); }

void Inspector::visitAgain() const { visited->visitAgain(getOriginal()); }
void Modifier::visitAgain() const { visited->visitAgain(getOriginal()); }
void Transform::visitAgain() const { visited->visitAgain(getOriginal()); }

void Visitor::print_context() const {
    std::ostream &out = std::cout;
    out << "Context:" << std::endl;
    auto ctx = getContext();
    if (ctx == nullptr) {
        out << "<nullptr>" << std::endl;
        return;
    }

    while (ctx != nullptr) {
        out << ctx->node << " (" << ctx->original << ")" << std::endl;
        ctx = ctx->parent;
    }
}

void Visitor::visitor_const_error() { BUG("const Visitor wants to change IR"); }
void Modifier::visitor_const_error() {
    BUG("Modifier called const visit function -- missing template "
        "instantiation in gen-tree-macro.h?");
}
void Transform::visitor_const_error() {
    BUG("Transform called const visit function -- missing template "
        "instantiation in gen-tree-macro.h?");
}

struct PushContext {
    Visitor::Context current;
    const Visitor::Context *&stack;
    bool saved_logging_disable;
    PushContext(const Visitor::Context *&stck, const IR::Node *node) : stack(stck) {
        saved_logging_disable = Log::Detail::enableLoggingInContext;
        if (const auto *annotated = node->to<IR::IAnnotated>()) {
            if (annotated->getAnnotation(IR::Annotation::debugLoggingAnnotation))
                Log::Detail::enableLoggingInContext = true;
        }
        current.parent = stack;
        current.node = current.original = node;
        current.child_index = 0;
        current.child_name = nullptr;
        current.depth = stack ? stack->depth + 1 : 1;
        assert(current.depth < 10000);  // stack overflow?
        stack = &current;
    }
    ~PushContext() {
        stack = current.parent;
        Log::Detail::enableLoggingInContext = saved_logging_disable;
    }
};

namespace {
class ForwardChildren : public Visitor {
    const ChangeTracker &visited;
    const IR::Node *apply_visitor(const IR::Node *n, const char * = 0) {
        if (const auto *result = visited.finalResult(n)) return result;
        return n;
    }

 public:
    explicit ForwardChildren(const ChangeTracker &v) : visited(v) {}
};
}  // namespace

const IR::Node *Modifier::apply_visitor(const IR::Node *n, const char *name) {
    if (ctxt) ctxt->child_name = name;
    if (n) {
        PushContext local(ctxt, n);
        switch (visited->try_start(n, visitDagOnce)) {
            case VisitStatus::Busy:
                n->apply_visitor_loop_revisit(*this);
                // FIXME -- should have a way of updating the node?  Needs to be decided
                // by the visitor somehow, but it is tough
                break;
            case VisitStatus::Done:
                n->apply_visitor_revisit(*this, visited->result(n));
                n = visited->result(n);
                break;
            default: {  // New or Revisit
                IR::Node *copy = n->clone();
                local.current.node = copy;
                if (!dontForwardChildrenBeforePreorder) {
                    ForwardChildren forward_children(*visited);
                    copy->visit_children(forward_children, name);
                }
                if (copy->apply_visitor_preorder(*this)) {
                    copy->visit_children(*this, name);
                    copy->apply_visitor_postorder(*this);
                }
                if (visited->finish(n, copy)) (n = copy)->validate();
                break;
            }
        }
    }
    if (ctxt)
        ctxt->child_index++;
    else
        visited.reset();
    return n;
}

const IR::Node *Inspector::apply_visitor(const IR::Node *n, const char *name) {
    if (ctxt) ctxt->child_name = name;
    if (n && !join_flows(n)) {
        PushContext local(ctxt, n);
        switch (visited->try_start(n, visitDagOnce)) {
            case VisitStatus::Busy:
                n->apply_visitor_loop_revisit(*this);
                break;
            case VisitStatus::Done:
                n->apply_visitor_revisit(*this);
                break;
            default:  // New or Revisit
                if (n->apply_visitor_preorder(*this)) {
                    n->visit_children(*this, name);
                    n->apply_visitor_postorder(*this);
                }
                visited->finish(n);
        }
        post_join_flows(n, n);
    }
    if (ctxt)
        ctxt->child_index++;
    else {
        visited.reset();
    }
    return n;
}

const IR::Node *Transform::apply_visitor(const IR::Node *n, const char *name) {
    if (ctxt) ctxt->child_name = name;
    if (n) {
        PushContext local(ctxt, n);
        switch (visited->try_start(n, visitDagOnce)) {
            case VisitStatus::Busy:
                n->apply_visitor_loop_revisit(*this);
                // FIXME -- should have a way of updating the node?  Needs to be decided
                // by the visitor somehow, but it is tough
                break;
            case VisitStatus::Done:
                n->apply_visitor_revisit(*this, visited->result(n));
                n = visited->result(n);
                break;
            default: {  // New or Revisit
                auto *copy = n->clone();
                local.current.node = copy;
                if (!dontForwardChildrenBeforePreorder) {
                    ForwardChildren forward_children(*visited);
                    copy->visit_children(forward_children, name);
                }
                bool save_prune_flag = prune_flag;
                prune_flag = false;
                bool extra_clone = false;
                const IR::Node *preorder_result = copy->apply_visitor_preorder(*this);
                assert(preorder_result != n);  // should never happen
                const IR::Node *final_result = preorder_result;
                if (preorder_result != copy) {
                    // FIXME -- not safe if the visitor resurrects the node (which it shouldn't)
                    // if (copy->id == IR::Node::currentId - 1)
                    //     --IR::Node::currentId;
                    if (!preorder_result) {
                        prune_flag = true;
                    } else if (visited->done(preorder_result)) {
                        final_result = visited->result(preorder_result);
                        prune_flag = true;
                    } else {
                        extra_clone = true;
                        auto status =
                            visited->try_start(preorder_result, visited->shouldVisitOnce(n));
                        // Sanity check for IR loops
                        if (status == VisitStatus::Busy) BUG("IR loop detected ");
                        local.current.node = copy = preorder_result->clone();
                    }
                }
                if (!prune_flag) {
                    copy->visit_children(*this, name);
                    final_result = copy->apply_visitor_postorder(*this);
                }
                prune_flag = save_prune_flag;
                if (final_result == copy && final_result != preorder_result &&
                    *final_result == *preorder_result)
                    final_result = preorder_result;
                if (visited->finish(n, final_result) && (n = final_result))
                    final_result->validate();
                if (extra_clone) visited->finish(preorder_result, final_result);
                break;
            }
        }
    }
    if (ctxt)
        ctxt->child_index++;
    else
        visited.reset();
    return n;
}

void Inspector::revisit_visited() { visited->revisit_visited(); }
bool Inspector::visit_in_progress(const IR::Node *n) const { return visited->busy(n); }
void Modifier::revisit_visited() { visited->revisit_visited(); }
bool Modifier::visit_in_progress(const IR::Node *n) const { return visited->busy(n); }
void Transform::revisit_visited() { visited->revisit_visited(); }
bool Transform::visit_in_progress(const IR::Node *n) const { return visited->busy(n); }

#define DEFINE_VISIT_FUNCTIONS(CLASS, BASE)                                                        \
    bool Modifier::preorder(IR::CLASS *n) { return preorder(static_cast<IR::BASE *>(n)); }         \
    void Modifier::postorder(IR::CLASS *n) { postorder(static_cast<IR::BASE *>(n)); }              \
    void Modifier::revisit(const IR::CLASS *o, const IR::CLASS *n) {                               \
        revisit(static_cast<const IR::BASE *>(o), static_cast<const IR::BASE *>(n));               \
    }                                                                                              \
    void Modifier::loop_revisit(const IR::CLASS *o) {                                              \
        loop_revisit(static_cast<const IR::BASE *>(o));                                            \
    }                                                                                              \
    bool Inspector::preorder(const IR::CLASS *n) {                                                 \
        return preorder(static_cast<const IR::BASE *>(n));                                         \
    }                                                                                              \
    void Inspector::postorder(const IR::CLASS *n) { postorder(static_cast<const IR::BASE *>(n)); } \
    void Inspector::revisit(const IR::CLASS *n) { revisit(static_cast<const IR::BASE *>(n)); }     \
    void Inspector::loop_revisit(const IR::CLASS *n) {                                             \
        loop_revisit(static_cast<const IR::BASE *>(n));                                            \
    }                                                                                              \
    const IR::Node *Transform::preorder(IR::CLASS *n) {                                            \
        return preorder(static_cast<IR::BASE *>(n));                                               \
    }                                                                                              \
    const IR::Node *Transform::postorder(IR::CLASS *n) {                                           \
        return postorder(static_cast<IR::BASE *>(n));                                              \
    }                                                                                              \
    void Transform::revisit(const IR::CLASS *o, const IR::Node *n) {                               \
        return revisit(static_cast<const IR::BASE *>(o), n);                                       \
    }                                                                                              \
    void Transform::loop_revisit(const IR::CLASS *o) {                                             \
        return loop_revisit(static_cast<const IR::BASE *>(o));                                     \
    }

IRNODE_ALL_SUBCLASSES(DEFINE_VISIT_FUNCTIONS)
#undef DEFINE_VISIT_FUNCTIONS

void ControlFlowVisitor::init_join_flows(const IR::Node *root) {
    if (!dynamic_cast<Inspector *>(static_cast<Visitor *>(this)))
        BUG("joinFlows only works for Inspector passes currently, not Modifier or Transform");
    if (flow_join_points)
        flow_join_points->clear();
    else
        flow_join_points = new std::remove_reference<decltype(*flow_join_points)>::type;
    applySetupJoinPoints(root);
#if DEBUG_FLOW_JOIN
    erase_if(*flow_join_points,
             [this](flow_join_points_t::value_type &el) { return el.second.count == 0; });
#endif
    erase_if(*flow_join_points,
             [this](flow_join_points_t::value_type &el) { return filter_join_point(el.first); });
}

bool ControlFlowVisitor::join_flows(const IR::Node *n) {
    if (!flow_join_points || !flow_join_points->count(n)) return false;  // not a flow join point
    auto &status = flow_join_points->at(n);
#if DEBUG_FLOW_JOIN
    status.parents[ctxt ? ctxt->original : nullptr].visited++;
#endif
    // BUG_CHECK(status.count >= 0, "join point reached too many times");
    // FIXME -- this means that we calculated the wrong number of parents for a
    // join point, and completed the join sooner than we should have.  This can
    // happen if there are recursive calls in the visitor and the differing order
    // between SetupJoinPoints and the main visitor means that the recursion is
    // seen differently.  Might be possible to fix this by careful use of
    // loop_revisit, but if not, we might as well just merge what we have.
    // Also caused by returning true below and revisiting the nodes that we've already
    // visited (so calling join_flows for their children more times than expected)
    // To really fix we need a way of dealing with loops and traversing the loop
    // repeatedly until a fixed point is reached.

    // Decrement the number of upstream edges yet to be traversed.  If none
    // remain, merge and return false to visit this node.
    if (--status.count < 0) {
        flow_merge(*status.vclone);
        // perhaps should return 'true' if count < -1 -- the case above where we're visiting
        // again after we thought we'd finished.  Returning 'false' will revisit all the children
        // which will at least let the visitor know something is going on.
        return false;
    }
    if (status.vclone) {
        // If there are still unvisited upstream edges but this is not the
        // first time this node has been reached, merge this visitor with
        // the accumulator (status.first)
        status.vclone->flow_merge(*this);
    } else {
        // Otherwise, this is the first time this node has been visited.
        // Clone this visitor and store it as the initial accumulator
        // value.
        status.vclone = clone();
    }
    if (BackwardsCompatibleBroken) {
        // We've reached a join point and not all parents have been visited.  Old behavior
        // was just to punt at this point (don't visit yet), which results in incorrect info
        // for successors of the current parent.  But some cases appear to depend on this.
        return true;
    }
    bool delta = true;
    while (delta && status.count >= 0) {
        delta = false;
        for (auto *sl = split_link; sl; sl = sl->prev) {
            if (sl->ready()) {
                sl->do_visit();  //  visit some parallel stuff;
                delta = true;
                break;
            }
        }
    }
    BUG_CHECK(status.count < 0 && status.done, "SplitFlow::do_visit failed to finish node");
    if (status.done) flow_copy(*status.vclone);
    return true;
}

void ControlFlowVisitor::post_join_flows(const IR::Node *n, const IR::Node *) {
    if (!flow_join_points || !flow_join_points->count(n)) return;  // not a flow join point
    auto &status = flow_join_points->at(n);
    BUG_CHECK(!status.done || status.count < -1, "flow join point visited more than once!: %s", n);
    status.done = true;
    status.vclone->flow_copy(*this);
}

bool Inspector::check_clone(const Visitor *v) {
    auto *t = dynamic_cast<const Inspector *>(v);
    BUG_CHECK(t && t->visited == visited, "Clone failed to copy base object");
    return Visitor::check_clone(v);
}
bool Modifier::check_clone(const Visitor *v) {
    auto *t = dynamic_cast<const Modifier *>(v);
    BUG_CHECK(t && t->visited == visited, "Clone failed to copy base object");
    return Visitor::check_clone(v);
}
bool Transform::check_clone(const Visitor *v) {
    auto *t = dynamic_cast<const Transform *>(v);
    BUG_CHECK(t && t->visited == visited, "Clone failed to copy base object");
    return Visitor::check_clone(v);
}

ControlFlowVisitor &ControlFlowVisitor::flow_clone() {
    auto *rv = clone();
    BUG_CHECK(rv->check_clone(this), "Clone failed to copy visitor type");
    return *rv;
}

IRNODE_ALL_NON_TEMPLATE_CLASSES(DEFINE_APPLY_FUNCTIONS, , , )

#define DEFINE_VISIT_FUNCTIONS(CLASS, BASE)                                      \
    void Visitor::visit(const IR::CLASS *&n, const char *name) {                 \
        auto t = apply_visitor(n, name);                                         \
        n = (t ? t->to<IR::CLASS>() : nullptr);                                  \
        if (t && !n) BUG("visitor returned non-" #CLASS " type: %1%", t);        \
    }                                                                            \
    void Visitor::visit(const IR::CLASS *const &n, const char *name) {           \
        /* This function needed solely due to order of declaration issues */     \
        visit(static_cast<const IR::Node *const &>(n), name);                    \
    }                                                                            \
    void Visitor::visit(const IR::CLASS *&n, const char *name, int cidx) {       \
        if (ctxt) ctxt->child_index = cidx;                                      \
        auto t = apply_visitor(n, name);                                         \
        n = (t ? t->to<IR::CLASS>() : nullptr);                                  \
        if (t && !n) BUG("visitor returned non-" #CLASS " type: %1%", t);        \
    }                                                                            \
    void Visitor::visit(const IR::CLASS *const &n, const char *name, int cidx) { \
        /* This function needed solely due to order of declaration issues */     \
        visit(static_cast<const IR::Node *const &>(n), name, cidx);              \
    }
IRNODE_ALL_SUBCLASSES(DEFINE_VISIT_FUNCTIONS)
#undef DEFINE_VISIT_FUNCTIONS

// Everything after this is for debugging or cleaner logging

std::ostream &operator<<(std::ostream &out, const IR::Vector<IR::Expression> *v) {
    return v ? out << *v : out << "<null>";
}

#if HAVE_CXXABI_H
cstring Visitor::demangle(const char *str) {
    int status;
    cstring rv;
    if (char *n = abi::__cxa_demangle(str, 0, 0, &status)) {
        rv = cstring(n);
        free(n);
    } else {
        rv = cstring(str);
    }
    return rv;
}
#else
#warning "No name demangling available; class names in logs will be mangled"
cstring Visitor::demangle(const char *str) { return cstring(str); }
#endif

#if HAVE_LIBGC
/** There's a bad interaction between the garbage collector and GCC's exception handling --
 * the exception support code in glibstdc++ (specifically __cxa_allocate_exception) allocates
 * space for exceptions being throw with malloc (NOT with ::operrator new for some reason),
 * and the garbage collector does not scan the malloc heap for roots when garbage collecting
 * (it knows nothing of its existance).  As a result, if you throw an exception AND that
 * exception object contains pointers to things on the GC heap, AND GC is triggered while
 * the exception is being thrown, AND those are the only pointers to those things, then
 * the objects might be garbage collected, leaving danglins pointers in the exception object.
 *
 * To avoid this problem, at least for backtracking exceptions, we track exception objects that
 * get created NOT on the GC heap and treat them as roots.
 */

static std::map<const Backtrack::trigger *, size_t> trigger_gc_roots;
#endif /* HAVE_LIBGC */

void Backtrack::trigger::register_for_gc(size_t
#if HAVE_LIBGC
                                             sz
#endif /* HAVE_LIBGC */
) {
#if HAVE_LIBGC
    if (!GC_is_heap_ptr(this)) {
        trigger_gc_roots[this] = sz;
        GC_add_roots(this, reinterpret_cast<char *>(this) + sz);
    }
#endif /* HAVE_LIBGC */
}

Backtrack::trigger::~trigger() {
#if HAVE_LIBGC
    if (auto sz = ::P4::get(trigger_gc_roots, this)) {
        GC_remove_roots(this, reinterpret_cast<char *>(this) + sz);
        trigger_gc_roots.erase(this);
    }
#endif /* HAVE_LIBGC */
}

std::ostream &operator<<(std::ostream &out, const Backtrack::trigger &trigger) {
    trigger.dbprint(out);
    return out;
}

std::ostream &operator<<(std::ostream &out, const SplitFlowVisit_base &split) {
    split.dbprint(out);
    return out;
}

std::ostream &operator<<(std::ostream &out, const ControlFlowVisitor::flow_join_info_t &info) {
    if (info.vclone) out << Visitor::demangle(typeid(*info.vclone).name()) << " ";
    out << "count=" << info.count << "  done=" << info.done;
#if DEBUG_FLOW_JOIN
    using namespace P4::DBPrint;
    auto flags = dbgetflags(out);
    out << Brief;
    for (auto &i : info.parents) {
        out << Log::endl
            << "  " << *i.first << " [" << i.first->id << "] " << "exist=" << i.second.exist
            << " visited=" << i.second.visited;
    }
    dbsetflags(out, flags);
#endif
    return out;
}

std::ostream &operator<<(std::ostream &out, const ControlFlowVisitor::flow_join_points_t &fjp) {
    using namespace P4::DBPrint;
    auto flags = dbgetflags(out);
    out << Brief;
    bool first = true;
    for (auto &jp : fjp) {
        if (!first) out << "\n";
        out << "[" << jp.first->id << "] " << *jp.first << ": " << jp.second;
        first = false;
    }
    dbsetflags(out, flags);
    return out;
}

void dump(const ControlFlowVisitor::flow_join_info_t &info) { std::cout << info << std::endl; }
void dump(const ControlFlowVisitor::flow_join_info_t *info) { std::cout << *info << std::endl; }
void dump(const ControlFlowVisitor::flow_join_points_t &fjp) { std::cout << fjp << std::endl; }
void dump(const ControlFlowVisitor::flow_join_points_t *fjp) { std::cout << *fjp << std::endl; }
void dump(const SplitFlowVisit_base *split) {
    while (split) {
        std::cout << *split << std::endl;
        split = split->prev;
    }
}

}  // namespace P4
