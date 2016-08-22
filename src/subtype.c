// This file is a part of Julia. License is MIT: http://julialang.org/license

/*
  subtyping predicate
*/
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#ifdef _OS_WINDOWS_
#include <malloc.h>
#endif
#include "julia.h"
#include "julia_internal.h"

typedef struct {
    int depth;
    int8_t more;
    int stacksize;
    uint32_t stack[10];
} jl_unionstate_t;

typedef struct _varbinding {
    jl_tvar_t *tv;
    jl_value_t *lb;
    jl_value_t *ub;
    int8_t right;
    struct _varbinding *prev;
} jl_varbinding_t;

typedef struct {
    jl_varbinding_t *vars;
    jl_unionstate_t Lunions;
    jl_unionstate_t Runions;
    int8_t outer;
} jl_stenv_t;

// state manipulation utilities

static jl_varbinding_t *lookup(jl_stenv_t *e, jl_tvar_t *v)
{
    jl_varbinding_t *b = e->vars;
    while (b != NULL) {
        if (b->tv == v) return b;
        b = b->prev;
    }
    return b;
}

static int statestack_get(jl_unionstate_t *st, int i)
{
    assert(i < st->stacksize);
    return (st->stack[i>>5] & (1<<(i&31))) != 0;
}

static void statestack_set(jl_unionstate_t *st, int i, int val)
{
    assert(i < st->stacksize);
    if (val)
        st->stack[i>>5] |= (1<<(i&31));
    else
        st->stack[i>>5] &= ~(1<<(i&31));
}

static void statestack_push(jl_unionstate_t *st, int val)
{
    st->stacksize++;
    assert(st->stacksize <= sizeof(st->stack)*8);
    statestack_set(st, st->stacksize-1, val);
}

static void statestack_pop(jl_unionstate_t *st)
{
    assert(st->stacksize > 0);
    st->stacksize--;
}

// main subtyping algorithm

static int subtype_union(jl_value_t *t, jl_uniontype_t *u, jl_stenv_t *e, int8_t R, jl_unionstate_t *state)
{
    e->outer = 0;
    if (state->depth >= state->stacksize) {
        state->more = 1;
        return 1;
    }
    int ui = statestack_get(state, state.depth);
    state->depth++;
    int choice = ui==0 ? u->a : u->b;
    return R ? subtype(t, choice, e) : subtype(choice, t, e);
}

static int var_lt(jl_tvar_t *b, jl_value_t *a, jl_stenv_t *e)
{
    e->outer = 0;
    jl_varbinding_t *bb = lookup(e, b);
    if (!bb->right)  // check ∀b . b<:a
        return subtype(bb->ub, a, e);
    if (!subtype(bb->lb, a, e))
        return 0;
    // for contravariance we would need to compute a meet here, but
    // because of invariance bb.ub ⊓ a == a here always. however for this
    // to work we need to compute issub(left,right) before issub(right,left),
    // since otherwise the issub(a, bb.ub) check in var_gt becomes vacuous.
    bb->ub = a;  // meet(bb->ub, a)
    return 1;
}

static jl_value_t *simple_join(jl_value_t *a, jl_value_t *b)
{
    if (a == jl_bottom_type || b == jl_any_type || a == b)
        return b;
    if (b == jl_bottom_type || a == jl_any_type)
        return a;
    return jl_new_struct(jl_uniontype_type, a, b);
}

static int var_gt(jl_tvar_t *b, jl_value_t *a, jl_stenv_t *e)
{
    e->outer = 0;
    jl_varbinding_t *bb = lookup(e, b);
    if (!bb->right)  // check ∀b . b>:a
        return subtype(a, bb->lb, e);
    if (!subtype(a, bb->ub, e))
        return 0;
    bb->lb = simple_join(bb->lb, a);
    return 1;
}

static jl_unionall_t *rename(jl_unionall_t *u)
{
    jl_tvar_t *t = jl_new_typevar(u->var->name, u->var->lb, u->var->ub);
    JL_GC_PUSH1(&t);
    u = jl_instantiate_unionall(u, t);
    JL_GC_POP();
    return u;
}

static int subtype_unionall(jl_value_t *t, jl_unionall_t *u, jl_stenv_t *e, int8_t R)
{
    int outer = e->outer;
    JL_GC_PUSH1(&u);
    if (lookup(e, u->var))
        u = rename(u);
    jl_varbinding_t vb;
    jl_varbinding_t *pvb;
    if (outer)
        pvb = malloc(sizeof(jl_varbinding_t));
    else
        pvb = &vb;
    pvb->tv = u->var;
    pvb->lb = u->var->lb;
    pvb->ub = u->var->ub;
    pvb->right = R;
    pvb->prev = e->vars;
    e->vars = pvb;
    int ans = R ? subtype(t, u->body, e) : subtype(u->body, t, e);
    if (!outer)
        e->vars = pvb->prev;
    JL_GC_POP();
    return ans;
}

static int jl_is_type(jl_value_t *x)
{
    return jl_is_datatype(x) || jl_is_uniontype(x) || jl_is_unionall(x) || x == jl_bottom_type;
}

static int subtype(jl_value_t *x, jl_value_t *y, jl_stenv_t *e)
{
    // take apart unions before handling vars
    if (jl_is_uniontype(x)) {
        if (jl_is_uniontype(y))
            return subtype_union(x, y, e, 1, &e->Runions);
        if (jl_is_unionall(y))
            return subtype_unionall(x, (jl_unionall_t*)y, e, 1);
        return subtype_union(y, x, e, 0, &e->Lunions);
    }
    if (jl_is_uniontype(y)) {
        //if (x == ((jl_uniontype_t*)y)->a || x == ((jl_uniontype_t*)y)->b)
        //    return 1;
        if (jl_is_unionall(x))
            return subtype_unionall(y, (jl_unionall_t*)x, e, 0);
        return subtype_union(x, y, e, 1, &e->Runions);
    }
    if (jl_is_typevar(x)) {
        if (jl_is_typevar(y)) {
            e->outer = 0;
            if (x == y) return 1;
            jl_varbinding_t *xx = lookup(e, x);
            jl_varbinding_t *yy = lookup(e, y);
            if (xx->right) {
                if (yy->right) {
                    // this is a bit odd, but seems necessary to make this case work:
                    // (UnionAll x<:T<:x RefT{RefT{T}}) == RefT{UnionAll x<:T<:x RefT{T}}
                    return subtype(yy->ub, yy->lb, e);
                }
                return var_lt((jl_tvar_t*)x, y, e);
            }
            else if (!yy->right) {   // check ∀x,y . x<:y
                // the bounds of left-side variables never change, and can only lead
                // to other left-side variables, so using || here is safe.
                return subtype(xx->ub, y, e) || subtype(x, yy->lb, e);
            }
            else {
                return var_gt((jl_tvar_t*)y, x, e);
            }
        }
        return var_lt((jl_tvar_t*)x, y, e);
    }
    if (jl_is_typevar(y))
        return var_gt((jl_tvar_t*)y, x, e);
    if (jl_is_unionall(y))
        return subtype_unionall(x, (jl_unionall_t*)y, e, 1);
    if (jl_is_unionall(x))
        return subtype_unionall(y, (jl_unionall_t*)x, e, 0);
    if (jl_is_datatype(x) && jl_is_datatype(y)) {
        e->outer = 0;
        if (x == y) return 1;
        if (y == jl_any_type) return 1;
        jl_datatype_t *xd = (jl_datatype_t*)x, *yd = (jl_datatype_t*)y;
        while (xd != jl_any_type && xd->name != yd->name)
            xd = xd->super;
        if (xd == jl_any_type) return 0;
        size_t lx = jl_nparams(xd);
        if (jl_is_tuple_type(xd)) {
            size_t ly = jl_nparams(yd);
            if (lx == 0 && ly == 0)
                return 1;
            if (ly == 0)
                return 0;
            size_t i=0, j=0;
            int vx=0, vy=0;
            while (i < lx) {
                if (j >= ly) return 0;
                jl_value_t *xi = jl_tparam(xd, i), *yi = jl_tparam(yd, j);
                if (jl_is_vararg_type(xi)) {
                    vx = 1;
                    xi = jl_tparam0(xi);
                }
                if (jl_is_vararg_type(yi)) {
                    vy = 1;
                    yi = jl_tparam0(yi);
                }
                if (!subtype(xi, yi, e))
                    return 0;
                i++;
                if (j < ly-1 || !vy)
                    j++;
            }
            return (lx==ly && vx==vy) || (vy && (lx >= (vx ? ly : (ly-1))));
        }
        size_t i;
        for (i=0; i < lx; i++) {
            jl_value_t *xi = jl_tparam(xd, i), *yi = jl_tparam(yd, i);
            if (!(xi == yi || (subtype(xi, yi, e) && subtype(yi, xi, e))))
                return 0;
        }
        return 1;
    }
    if (jl_is_type(x) && jl_is_type(y))
        return x == jl_bottom_type || x == y;
    return x == y || jl_egal(x, y);
}

static int exists_subtype(jl_value_t *x, jl_value_t *y, jl_stenv_t *e, int8_t anyunions)
{
    int exists;
    for (exists=0; exists <= anyunions; exists++) {
        if (e->Runions.stacksize > 0)
            statestack_set(&e->Runions, e->Runions.stacksize-1, exists);
        e->Lunions.depth = e->Runions.depth = 0;
        e->Lunions.more = e->Runions.more = 0;
        int found = subtype(x, y, e);
        if (e->Lunions.more)
            return 1;
        if (e->Runions.more) {
            statestack_push(&e->Runions, 0);
            found = exists_subtype(x, y, e, 1);
            statestack_pop(&e->Runions);
        }
        if (found) return 1;
    }
    return 0;
}

static int forall_exists_subtype(jl_value_t *x, jl_value_t *y, jl_stenv_t *e, int8_t anyunions)
{
    int forall;
    for (forall=0; forall <= anyunions; forall++) {
        if (e->Lunions.stacksize > 0)
            statestack_set(&e->Lunions, e->Lunions.stacksize-1, forall);
        if (!exists_subtype(x, y, e, 0))
            return 0;
        if (e->Lunions.more) {
            statestack_push(&e->Lunions, 0);
            int sub = forall_exists_subtype(x, y, e, 1);
            statestack_pop(&e->Lunions);
            if (!sub) return 0;
        }
    }
    return 1;
}

JL_DLLEXPORT int jl_subtype_env(jl_value_t *x, jl_value_t *y, jl_stenv_t *e)
{
    e->vars = NULL;
    e->outer = 1;
    e->Lunions.depth = 0;      e->Runions.depth = 0;
    e->Lunions.more = 0;       e->Runions.more = 0;
    e->Lunions.stacksize = 0;  e->Runions.stacksize = 0;
    return forall_exists_subtype(x, y, e, 0);
}

JL_DLLEXPORT int jl_subtype(jl_value_t *x, jl_value_t *y)
{
    jl_stenv_t e;
    return jl_subtype_env(x, y, &e);
}
