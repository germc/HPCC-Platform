/*##############################################################################

    Copyright (C) 2011 HPCC Systems.

    All rights reserved. This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU Affero General Public License as
    published by the Free Software Foundation, either version 3 of the
    License, or (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Affero General Public License for more details.

    You should have received a copy of the GNU Affero General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
############################################################################## */
#include "jliball.hpp"

#if defined(_DEBUG) && defined(_WIN32) && !defined(USING_MPATROL)
 #undef new
 #define new new(_NORMAL_BLOCK, __FILE__, __LINE__)
#endif

#include "hql.hpp"
#include "platform.h"
#include "jlib.hpp"
#include "jmisc.hpp"
#include "jstream.ipp"
#include "hql.hpp"
#include "hqlexpr.hpp"
#include "hqlexpr.ipp"
#include "hqltrans.ipp"
#include "hqlthql.hpp"
#include "hqlutil.hpp"
#include "hqlpmap.hpp"
#include "hqlerrors.hpp"
#include "hqlerror.hpp"

//#define VERYIFY_OPTIMIZE
#ifdef _DEBUG
 #define TRACK_ACTIVE_EXPRESSIONS               // Useful for examining the transform path in a debugger
#endif

static unsigned transformerDepth;
static CLargeMemoryAllocator * transformerHeap;

#define ALLOCATOR_MIN_PAGES     10

static IHqlExpression * alreadyVisitedMarker;
static unsigned maxMapCount;

static PointerArrayOf<HqlTransformerInfo> allTransformers;

#ifdef TRACK_ACTIVE_EXPRESSIONS
HqlExprCopyArray activeExprStack;
#endif

MODULE_INIT(INIT_PRIORITY_STANDARD)
{
#ifdef OPTIMIZE_TRANSFORM_ALLOCATOR
    transformerHeap = new CLargeMemoryAllocator(0xffffffff, 0xff00, false);
#endif
    alreadyVisitedMarker = createAttribute(_alreadyVisitedMarker_Atom);
    return true;
}
MODULE_EXIT()
{
    if (maxMapCount)
        printf("MaxMap = %d\n", maxMapCount);
    ::Release(alreadyVisitedMarker);
#ifdef OPTIMIZE_TRANSFORM_ALLOCATOR
    delete transformerHeap;
#endif
}

unsigned HqlTransformStats::globalDepth;

HqlTransformStats::HqlTransformStats() 
{ 
    clear();
}

void HqlTransformStats::clear()
{
    numAnalyse = 0; 
    numAnalyseCalls = 0; 
    numTransforms = 0; 
    numTransformsSame = 0; 
    numTransformCalls = 0; 
    numTransformCallsSame = 0; 
    numTransformSelects = 0; 
    numTransformSelectsSame = 0;
    depth = 0;
    maxDepth = 0;
    maxGlobalDepth = 0;
#ifdef TRANSFORM_STATS_TIME
    totalTime = 0;
    childTime = 0;
    recursiveTime = 0;
#endif
#ifdef TRANSFORM_STATS_OPS
    _clear(transformCount);
#endif
}

void HqlTransformStats::beginTransform()
{
    numTransformCalls++;
    depth++;
    if (depth > maxDepth)
        maxDepth = depth;
    globalDepth++;
    if (globalDepth > maxGlobalDepth)
    {
        maxGlobalDepth = globalDepth;
    }
}


void HqlTransformStats::endMatchTransform(IHqlExpression * expr, IHqlExpression * match)
{
    if (expr == match)
        numTransformCallsSame++;
    depth--;
    globalDepth--;
}

void HqlTransformStats::endNewTransform(IHqlExpression * expr, IHqlExpression * transformed)
{
    numTransforms++;
    depth--;
    globalDepth--;
    if (expr == transformed)
        numTransformsSame++;
#ifdef TRANSFORM_STATS_OPS
    transformCount[expr->getOperator()]++;
#endif
}


void HqlTransformStats::add(const HqlTransformStats & other) 
{ 
    numAnalyse += other.numAnalyse;
    numAnalyseCalls += other.numAnalyseCalls;
    numTransforms += other.numTransforms;
    numTransformsSame += other.numTransformsSame;
    numTransformCalls += other.numTransformCalls;
    numTransformCallsSame += other.numTransformCallsSame;
    numTransformSelects += other.numTransformSelects;
    numTransformSelectsSame += other.numTransformSelectsSame;
    if (maxDepth < other.maxDepth)
        maxDepth = other.maxDepth;
    if (maxGlobalDepth < other.maxGlobalDepth)
        maxGlobalDepth = other.maxGlobalDepth;
#ifdef TRANSFORM_STATS_TIME
    totalTime += other.totalTime;
    childTime += other.childTime;
    recursiveTime += other.recursiveTime;
#endif
#ifdef TRANSFORM_STATS_OPS
    for (unsigned i=0; i < no_last_pseudoop; i++)
        transformCount[i] += other.transformCount[i];
#endif
}

StringBuffer & HqlTransformStats::getText(StringBuffer & out) const
{ 
#ifdef TRANSFORM_STATS_TIME
    out.append(" ti:").append(totalTime-(childTime-recursiveTime)).append(" tt:").append(totalTime).append(" tr:").append(recursiveTime);
#endif
    out.append(" a:").append(numAnalyse);
    out.append(" ac:").append(numAnalyseCalls);
    out.append(" t:").append(numTransforms).append(" ts:").append(numTransformsSame);
    out.append(" tc:").append(numTransformCalls).append(" tcs:").append(numTransformsSame+numTransformCallsSame);
    out.append(" ts:").append(numTransformSelects).append(" tss:").append(numTransformSelectsSame);
    out.append(" md:").append(maxDepth).append(" mgd:").append(maxGlobalDepth);

#ifdef TRANSFORM_STATS_OPS
    if (numAnalyse || numTransforms)
    {
        for (unsigned i=0; i < no_last_pseudoop; i++)
        {
            if (transformCount[i])
                out.appendf("\"%s\",%d", getOpString((node_operator)i), transformCount[i]).newline();
        }
    }
#endif

    return out;
}


HqlTransformerInfo::HqlTransformerInfo(const char * _name)
{
#ifdef TRANSFORM_STATS
    allTransformers.append(this);
#endif
    name = _name;
}

HqlTransformerInfo::~HqlTransformerInfo()
{
#ifdef TRANSFORM_STATS
    StringBuffer s;
    if (getStatsText(s))
        printf(s.newline().str());
#endif
}

bool HqlTransformerInfo::getStatsText(StringBuffer & s) const
{
#ifdef TRANSFORM_STATS
    if (numInstances)
    {
        s.append("Transform: ").append(name).append(": i:").append(numInstances);
        stats.getText(s);
        return true;
    }
#endif
    return false;
}

void HqlTransformerInfo::resetStats()
{
#ifdef TRANSFORM_STATS
    numInstances = 0;
    stats.clear();
#endif
}


void dbglogTransformStats(bool reset)
{
    //Not at all thread safe, only meant for profiling
    StringBuffer s;
    ForEachItemIn(i, allTransformers)
    {
        HqlTransformerInfo * cur = allTransformers.item(i);
        if (cur->getStatsText(s.clear()))
        {
            DBGLOG("%s", s.str());
            if (reset)
                cur->resetStats();
        }
    }
}


static bool tracing = false;
extern HQL_API void setTransformTracing(bool ok) { tracing = ok; }
extern HQL_API bool isTransformTracing() { return tracing; }

//-----------------------------------------------------------------------------------------

//weird semantics for efficiency
//if the expression doesn't hide the selector then return 0
//if it does return the number of arguments that aren't hidden
unsigned activityHidesSelectorGetNumNonHidden(IHqlExpression * expr, IHqlExpression * selector)
{
#ifdef ENSURE_SELSEQ_UID
    return 0;
#endif
    if (!selector)
        return 0;
    node_operator op = selector->getOperator();
    if ((op != no_left) && (op != no_right))
        return 0;

    switch (getChildDatasetType(expr))
    {
    case childdataset_none:
    case childdataset_addfiles:
    case childdataset_merge:
    case childdataset_map:
    case childdataset_dataset_noscope:
    case childdataset_if:
    case childdataset_case:
    case childdataset_dataset:
    case childdataset_evaluate:
        return 0;
    case childdataset_datasetleft:
    case childdataset_left:
        if ((op == no_left) && recordTypesMatch(selector, expr->queryChild(0)))
        {
            switch (expr->getOperator())
            {
            case no_loop: case no_graphloop:
                return 2;
            }
            return 1;
        }
        return 0;
    case childdataset_leftright:
        if (op == no_left)
        {
            if (recordTypesMatch(selector, expr->queryChild(0)))
            {
                if (expr->getOperator() == no_normalize)
                    return 1;
                return 2;
            }
        }
        else
        {
            if (recordTypesMatch(selector, expr->queryChild(1)))
                return 2;
        }
        return 0;
    case childdataset_same_left_right:
    case childdataset_top_left_right:
    case childdataset_nway_left_right:
        if (recordTypesMatch(selector, expr->queryChild(0)))
            return 1;
        return 0;
    default:
        UNIMPLEMENTED;
    }
}


bool activityHidesRows(IHqlExpression * expr, IHqlExpression * selector)
{
    if (!selector)
        return 0;

    node_operator selectOp = selector->getOperator();
    switch (expr->getOperator())
    {
    case no_rollupgroup:
    case no_loop:
    case no_loop2:
    case no_graphloop:
    case no_filtergroup:
        return (selectOp == no_left);
    case no_combinegroup:
    case no_denormalizegroup:
        return (selectOp == no_right);
    }
    return false;
}

//---------------------------------------------------------------------------

static void insertScopeSymbols(IHqlScope * newScope, HqlExprArray const & symbols)
{
    ForEachItemIn(i, symbols)
        newScope->defineSymbol(LINK(&symbols.item(i)));
}


//---------------------------------------------------------------------------

static HqlTransformerBase * activeTransformer;      // not thread safe

#ifdef TRANSFORM_STATS_TIME
void HqlTransformerBase::beginTime()
{
    startTime = msTick();
    prev = activeTransformer;
    activeTransformer = this;
}

void HqlTransformerBase::endTime()
{
    stats.totalTime = (msTick() - startTime);
    if (prev)
        prev->noteChildTime(stats.totalTime, &info==&prev->info);
    activeTransformer = prev;
}
#endif

void HqlTransformerBase::noteMemory()
{
#ifdef TRANSFORM_STATS_MEMORY
    PROGLOG("Finish %s", info.name);
    PrintMemoryReport();
#endif
}


bool HqlTransformerBase::optimizedTransformChildren(IHqlExpression * expr, HqlExprArray & children)
{
    //Same as transformChildren(), but avoid appending to children array unless something has changed.
    unsigned numDone = children.ordinality();
    unsigned max = expr->numChildren();
    unsigned idx;
    for (idx = 0; idx < numDone; idx++)
    {
        if (&children.item(idx) != expr->queryChild(idx))
            break;
    }
    
    if (idx == numDone)
    {
        OwnedHqlExpr lastTransformedChild;
        for (;idx < max; idx++)
        {
            IHqlExpression * child = expr->queryChild(idx);
            lastTransformedChild.setown(transform(child));
            if (child != lastTransformedChild)
                break;
        }

        if (idx == max)
            return true;

        children.ensure(max);
        for (unsigned i=numDone; i < idx; i++)
            children.append(*LINK(expr->queryChild(i)));
        children.append(*lastTransformedChild.getClear());
        idx++;
    }
    else
    {
        children.ensure(max);
        idx = numDone;
    }

    for (;idx < max; idx++)
    {
        IHqlExpression * child = expr->queryChild(idx);
        children.append(*transform(child));
    }

    return false;
}

IHqlExpression * HqlTransformerBase::transformAlienType(IHqlExpression * expr)
{
    HqlExprArray newSymbols;
    HqlExprArray children;
    Linked<IHqlScope> scope = expr->queryScope();
    bool scopeSame = transformScope(newSymbols, scope);
    bool same = transformChildren(expr, children);
    if (!same || !scopeSame)
    {
        if (!scopeSame)
        {
            Owned<IHqlScope> newScope = createScope();
            insertScopeSymbols(newScope, newSymbols);
            scope.setown(closeScope(newScope.getClear()));
        }
        return createAlienType(expr->queryName(), scope.getClear(), children, LINK(expr->queryFunctionDefinition()));
    }
    return LINK(expr);
}


bool HqlTransformerBase::transformChildren(IHqlExpression * expr, HqlExprArray & children)
{
    unsigned numDone = children.ordinality();
    unsigned max = expr->numChildren();
    unsigned idx;
    bool same = true;

    for (idx = 0; idx < numDone; idx++)
        if (&children.item(idx) != expr->queryChild(idx))
            same = false;

    children.ensure(max);
    for (idx=numDone;idx<max;idx++)
    {
        IHqlExpression * child = expr->queryChild(idx);
        IHqlExpression * tchild = transform(child);
        children.append(*tchild);
        if (child != tchild)
            same = false;
    }
    return same;
}


IHqlExpression * HqlTransformerBase::transformField(IHqlExpression * expr)
{
    ITypeInfo * type = expr->queryType();
    OwnedITypeInfo newType = transformType(type);
    HqlExprArray children;
    if (type != newType)
    {
        transformChildren(expr, children);
        return createField(expr->queryName(), LINK(newType), children);
    }
    return completeTransform(expr, children);
}


bool HqlTransformerBase::transformScope(HqlExprArray & newSymbols, IHqlScope * scope)
{
    HqlExprArray symbols;
    scope->getSymbols(symbols);
//  symbols.sort(compareSymbolsByName);

    bool same = true;
    ForEachItemIn(idx, symbols)
    {
        IHqlExpression & cur = symbols.item(idx);
        OwnedHqlExpr transformed = transform(&cur);

        //Ensure the named symbol associated with the symbol is preserved (can be stripped by normalizer)
        if (!hasNamedSymbol(transformed) || (transformed->queryName() != cur.queryName()))
            transformed.setown(forceCloneSymbol(&cur, transformed));

        if (transformed != &cur)
            same = false;
        newSymbols.append(*transformed.getClear());
    }

    return same;
}


bool HqlTransformerBase::transformScope(IHqlScope * newScope, IHqlScope * oldScope)
{
    HqlExprArray newSymbols;
    bool same = transformScope(newSymbols, oldScope);
    insertScopeSymbols(newScope, newSymbols);
    return same;
}


IHqlExpression * HqlTransformerBase::transformScope(IHqlExpression * expr)
{
    IHqlScope * scope = expr->queryScope();
    HqlExprArray symbols, children;
    bool same = transformScope(symbols, scope);
    if (!transformChildren(expr, children))
        same = false;
    if (same)
        return LINK(expr);
    return queryExpression(scope->clone(children, symbols));
}


ITypeInfo * HqlTransformerBase::transformType(ITypeInfo * type)
{
    switch (type->queryModifier())
    {
    case typemod_original:
        {
            ITypeInfo * typeBase = type->queryTypeBase();
            Owned<ITypeInfo> newTypeBase = transformType(typeBase);
            IHqlExpression * original = static_cast<IHqlExpression *>(type->queryModifierExtra());
            OwnedHqlExpr transformedOriginal = transform(original);
            if ((typeBase == newTypeBase) && (original == transformedOriginal))
                return LINK(type);
            return makeOriginalModifier(newTypeBase.getClear(), transformedOriginal.getClear()); 
        }
    case typemod_indirect:
        {
            IHqlExpression * original = static_cast<IHqlExpression *>(type->queryModifierExtra());
            OwnedHqlExpr transformedOriginal = transform(original);
            if (original == transformedOriginal)
                return LINK(type);
            return makeModifier(transformedOriginal->getType(), typemod_indirect, LINK(transformedOriginal));
        }
    case typemod_none:
        break;
    default:
        {
            ITypeInfo * typeBase = type->queryTypeBase();
            Owned<ITypeInfo> newType = transformType(typeBase);
            if (typeBase == newType)
                return LINK(type);
            return cloneModifier(type, newType);
        }
    }
    switch (type->getTypeCode())
    {
    case type_alien:
        {
            IHqlExpression * typeExpr = queryExpression(type);
            OwnedHqlExpr newTypeExpr = transform(typeExpr);
            return newTypeExpr->getType();
        }
        break;
    case type_record:
        {
            IHqlExpression * record = queryExpression(type);
            OwnedHqlExpr newRecord = transform(record);
            return newRecord->getType();
        }
    case type_set:
        {
            ITypeInfo * childType = type->queryChildType();
            if (childType)
            {
                Owned<ITypeInfo> newChild = transformType(childType);
                if (childType != newChild)
                    return makeSetType(newChild.getClear());
            }
            break;
        }
    case type_scope:
        {
            IHqlExpression * scope = queryExpression(type);
            OwnedHqlExpr newScope = transform(scope);
            return newScope->getType();
        }
    case type_row:
    case type_transform:
    case type_pattern:
    case type_rule:
    case type_token:
    case type_groupedtable:
    case type_table:
        {
            ITypeInfo * childType = type->queryChildType();
            OwnedITypeInfo newChildType = safeTransformType(childType);
            return replaceChildType(type, newChildType);
        }
    }
    return LINK(type);
}


//---------------------------------------------------------------------------

QuickHqlTransformer::QuickHqlTransformer(HqlTransformerInfo & _info, IErrorReceiver * _errors) : HqlTransformerBase(_info)
{ 
    errors = _errors;
}

void QuickHqlTransformer::analyse(IHqlExpression * expr)
{
#ifdef TRANSFORM_STATS
    stats.numAnalyseCalls++;
#endif

    IHqlExpression * match = static_cast<IHqlExpression *>(expr->queryTransformExtra());
    if (match)
        return;

#ifdef TRANSFORM_STATS
    stats.numAnalyse++;
#endif

    doAnalyse(expr);
    expr->setTransformExtraUnlinked(alreadyVisitedMarker);
}

void QuickHqlTransformer::doAnalyse(IHqlExpression * expr)
{
    IHqlExpression * body = expr->queryBody(true);
    if (expr != body)
    {
        if (body)
            analyse(body);
    }
    else
    {
#ifdef TRANSFORM_STATS_OPS
        stats.transformCount[expr->getOperator()]++;
#endif
        doAnalyseBody(expr);
    }
}

void QuickHqlTransformer::doAnalyseBody(IHqlExpression * expr)
{
    unsigned max = expr->numChildren();
    for (unsigned i=0; i < max; i++)
        analyse(expr->queryChild(i));

    node_operator op = expr->getOperator();
    switch (op)
    {
    case no_call:
    case no_libraryscopeinstance:
        analyse(expr->queryDefinition());
        break;
    case no_getresult:
    case no_newtransform:
    case no_transform:
        {
            IHqlExpression * oldRecord = queryOriginalRecord(expr);
            if (oldRecord)
                analyse(oldRecord);
            break;
        }
    case no_scope:
    case no_virtualscope:
    case no_concretescope:
    case no_libraryscope:
    case no_forwardscope:
        {
            IHqlScope * scope = expr->queryScope();
            HqlExprArray oldsyms;
            scope->getSymbols(oldsyms);
            oldsyms.sort(compareSymbolsByName);
            for (unsigned idx = 0; idx < oldsyms.length(); idx++)
            {
                IHqlExpression *oldkid = &oldsyms.item(idx);
                analyse(oldkid);
            }
            break;
        }
    case no_type:
        {
            IHqlScope * scope = expr->queryScope();
            if (scope)
                analyse(scope);
            break;
        }
    case no_enum:
        analyse(expr->queryScope());
        break;
    case no_field:
        {
            ITypeInfo * type = expr->queryType();
            switch (type->getTypeCode())
            {
            case type_alien:
                analyse(queryExpression(type));
                break;
            case type_record:
                throwUnexpected();
            case type_row:
            case type_table:
            case type_groupedtable:
                analyse(queryOriginalRecord(expr));
                break;
            }
        }
        break;
    }
}


void QuickHqlTransformer::analyse(IHqlScope * scope)
{
    analyse(queryExpression(scope));
}

void QuickHqlTransformer::analyseArray(const HqlExprArray & exprs)
{
    ForEachItemIn(i, exprs)
        analyse(&exprs.item(i));
}

IHqlExpression * QuickHqlTransformer::transform(IHqlExpression * expr)
{
#ifdef TRANSFORM_STATS
    stats.beginTransform();
#endif

    IHqlExpression * match = static_cast<IHqlExpression *>(expr->queryTransformExtra());
    if (match && (match != alreadyVisitedMarker))
    {
#ifdef TRANSFORM_STATS
        stats.endMatchTransform(expr, match);
#endif
        return LINK(match);
    }

    IHqlExpression * ret = createTransformed(expr);

#ifdef TRANSFORM_STATS
    stats.endNewTransform(expr, ret);
#endif
    expr->setTransformExtra(ret);
    return ret;
}

void QuickHqlTransformer::transformArray(const HqlExprArray & in, HqlExprArray & out)
{
    ForEachItemIn(idx, in)
    {
        OwnedHqlExpr ret = transform(&in.item(idx));
        if (ret)
            unwindCommaCompound(out, ret);
    }
}


IHqlExpression * QuickHqlTransformer::createTransformed(IHqlExpression * expr)
{
    IHqlExpression * body = expr->queryBody(true);
    if (expr != body)
    {
        OwnedHqlExpr newbody = transform(body);
        if (body == newbody)
            return LINK(expr);
        return expr->cloneAnnotation(newbody);
    }
    return createTransformedBody(expr);
}

IHqlExpression * QuickHqlTransformer::createTransformedBody(IHqlExpression * expr)
{
    node_operator op = expr->getOperator();
    unsigned max = expr->numChildren();
    if (max == 0)
    {
        switch (op)
        {
        case no_scope:
        case no_privatescope:
        case no_virtualscope:
        case no_concretescope:
        case no_forwardscope:
        case no_type:
        case no_enum:
        case no_field:
        case no_call:
        case no_libraryscopeinstance:
        case no_param:
            break;
        default:
            return LINK(expr);
        }
    }

    HqlExprArray children;
    switch (op)
    {
    case no_call:
    case no_libraryscopeinstance:
        {
            IHqlExpression * funcDef = expr->queryDefinition();
            Owned<IHqlExpression> newFuncDef = transform(funcDef);
            if (funcDef != newFuncDef)
            {
                transformChildren(expr, children);
                return createReboundFunction(newFuncDef, children);
            }
            break;
        }
    case no_getresult:
        if (!expr->queryRecord())
            break;
    case no_newtransform:
    case no_transform:
    case no_rowsetrange:
    case no_datasetlist:
    case no_rowset:
        {
            ITypeInfo * type = expr->queryType();
            OwnedITypeInfo newType = transformType(type);
            if (type != newType)
            {
                transformChildren(expr, children);
                return createValue(op, newType.getClear(), children);
            }
            break;
        }
    case no_privatescope:
        throwUnexpected();
    case no_scope:
    case no_virtualscope:
    case no_concretescope:
    case no_libraryscope:
    case no_forwardscope:
        {
            IHqlScope * scope = expr->queryScope();
            bool same = transformChildren(expr, children);
            if (op == no_forwardscope)
            {
                scope = scope->queryResolvedScope(NULL);
                same = false;
            }
            HqlExprArray newsyms;
            if (!transformScope(newsyms, scope))
                same = false;

            if (same)
                return LINK(expr);

            return queryExpression(scope->clone(children, newsyms));
        }
    case no_type:
        return transformAlienType(expr);
    case no_enum:
        {
            IHqlScope * scope = expr->queryScope();
            Owned<IHqlScope> newScope = transform(scope);
            if (scope == newScope)
                return LINK(expr);
            return createEnumType(expr->getType(), newScope.getClear());
        }
    case no_field:
        {
            ITypeInfo * type = expr->queryType();
            OwnedITypeInfo newType = transformType(type);
            if (type != newType)
            {
                transformChildren(expr, children);
                return createField(expr->queryName(), LINK(newType), children);
            }
            break;
        }
    case no_funcdef:
        {
            if (transformChildren(expr, children))
                return LINK(expr);
            return createFunctionDefinition(expr->queryName(), children);
        }
    case no_param:
        {
            ITypeInfo * type = expr->queryType();
            Owned<ITypeInfo> newType = transformType(type);
            transformChildren(expr, children);
            if (type != newType)
                return createParameter(expr->queryName(), (unsigned)expr->querySequenceExtra(), newType.getClear(), children);
            break;
        }
    }

    return completeTransform(expr, children);
}


IHqlScope * QuickHqlTransformer::transform(IHqlScope * scope)
{
    IHqlExpression * scopeExpr = queryExpression(scope);
    return transform(scopeExpr)->queryScope();
}

void QuickHqlTransformer::setMapping(IHqlExpression * oldValue, IHqlExpression * newValue)
{
    loop
    {
        oldValue->setTransformExtra(newValue);
        IHqlExpression * body = oldValue->queryBody(true);
        if (oldValue == body)
            break;
        oldValue = body;
        newValue = newValue->queryBody(true);
    }
}

//-------------------------------------------------------------------------------------------------

//NB: Derived from QuickHqlTransformer since it is called before the tree is normalised
static HqlTransformerInfo quickExpressionReplacerInfo("QuickExpressionReplacer");
QuickExpressionReplacer::QuickExpressionReplacer() 
: QuickHqlTransformer(quickExpressionReplacerInfo, NULL) 
{
}

void QuickExpressionReplacer::setMapping(IHqlExpression * oldValue, IHqlExpression * newValue)
{
    loop
    {
        oldValue->setTransformExtra(newValue);
        IHqlExpression * body = oldValue->queryBody(true);
        if (body == oldValue)
            break;
        oldValue = body;
        newValue = newValue->queryBody(true);
    }
}

IHqlExpression * quickFullReplaceExpression(IHqlExpression * expr, IHqlExpression * oldValue, IHqlExpression * newValue)
{
    if (oldValue == newValue)
        return LINK(expr);
    if (expr == oldValue)
        return LINK(newValue);

    QuickExpressionReplacer map;
    map.setMapping(oldValue, newValue);
    return map.transform(expr);
}

IHqlExpression * quickFullReplaceExpressions(IHqlExpression * expr, const HqlExprArray & oldValues, const HqlExprArray & newValues)
{
    QuickExpressionReplacer map;
    ForEachItemIn(i, oldValues)
        map.setMapping(&oldValues.item(i), &newValues.item(i));
    return map.transform(expr);
}

//-------------------------------------------------------------------------------------------------

static HqlTransformerInfo quickExpressionLocatorInfo("QuickExpressionLocator");
class HQL_API QuickExpressionLocator : public QuickHqlTransformer
{
public:
    QuickExpressionLocator(IHqlExpression * _search) 
    : QuickHqlTransformer(quickExpressionLocatorInfo, NULL), search(_search)
    {
    }

    virtual IHqlExpression * createTransformedBody(IHqlExpression * expr)
    {
        assertex(expr != search);
        return QuickHqlTransformer::createTransformedBody(expr);
    }

protected:
    HqlExprAttr search;
};

extern void assertNoMatchingExpression(IHqlExpression * expr, IHqlExpression * search)
{
    QuickExpressionLocator locator(search->queryBody());
    OwnedHqlExpr ret = locator.transform(expr);
}



//-------------------------------------------------------------------------------------------------

static HqlTransformerInfo debugDifferenceAnalyserInfo("DebugDifferenceAnalyser");
DebugDifferenceAnalyser::DebugDifferenceAnalyser(_ATOM _search) : QuickHqlTransformer(debugDifferenceAnalyserInfo, NULL) 
{ 
    prev = NULL; 
    search = _search; 
}


void DebugDifferenceAnalyser::doAnalyse(IHqlExpression * expr)
{
    if (expr->queryName() == createIdentifierAtom("f_fuzzy") || expr->queryName() == createIdentifierAtom("f_exact"))
    {
        if (prev && prev->queryBody() != expr->queryBody())
        {
            debugFindFirstDifference(expr, prev);
            return;
        }
        else
            prev = expr;
    }
    QuickHqlTransformer::doAnalyse(expr);
}


//-------------------------------------------------------------------------------------------------

//NB: Derived from QuickHqlTransformer since it is called before the tree is normalised
static HqlTransformerInfo hqlSectionAnnotatorInfo("HqlSectionAnnotator");
HqlSectionAnnotator::HqlSectionAnnotator(IHqlExpression * sectionWorkflow) 
: QuickHqlTransformer(hqlSectionAnnotatorInfo, NULL) 
{
    HqlExprArray args;
    args.append(*LINK(sectionWorkflow->queryChild(0)));
    ForEachChildFrom(i, sectionWorkflow, 1)
    {
        IHqlExpression * cur = sectionWorkflow->queryChild(i);
        if (cur->isAttribute())
            args.append(*LINK(cur));
    }
    sectionAttr.setown(createExprAttribute(sectionAtom, args));
}

void HqlSectionAnnotator::noteInput(IHqlExpression * expr)
{
    setMapping(expr, expr);
}


IHqlExpression * HqlSectionAnnotator::createTransformedBody(IHqlExpression * expr)
{
    OwnedHqlExpr ret = QuickHqlTransformer::createTransformedBody(expr);
    if (!ret->isDataset() || !okToAddAnnotation(expr))
        return ret.getClear();

    HqlExprArray args;
    args.append(*LINK(sectionAttr));
    return createMetaAnnotation(ret.getClear(), args);
}

//---------------------------------------------------------------------------

ANewTransformInfo::ANewTransformInfo(IHqlExpression * _original)
{
    original = _original;
    lastPass = (byte) -1;
    flags = 0;
}


//---------------------------------------------------------------------------

inline bool quickTransformMustTraverse(IHqlExpression * expr)
{
    return containsNonActiveDataset(expr) || containsActiveNonSelector(expr) || containsMustHoist(expr);
}

NewHqlTransformer::NewHqlTransformer(HqlTransformerInfo & _info) : ANewHqlTransformer(_info)
{
    analyseFlags = TFunconditional;
    optimizeFlags = 0;
    pass = (unsigned) -2;
}


bool NewHqlTransformer::alreadyVisited(IHqlExpression * expr)
{
    ANewTransformInfo * extra = queryTransformExtra(expr);
    if (extra->lastPass == pass)
        return true;

#ifdef TRANSFORM_STATS
    stats.numAnalyse++;
#endif

    extra->lastPass = pass;
    return false;
}


void NewHqlTransformer::analyse(IHqlExpression * expr, unsigned _pass)
{
    pass = _pass;
    analyseExpr(expr);
    pass = (unsigned) -1;
}

void NewHqlTransformer::analyseArray(const HqlExprArray & exprs, unsigned _pass)
{
    pass = _pass;
    ForEachItemIn(idx, exprs)
        analyseExpr(&exprs.item(idx));
    pass = (unsigned) -1;
}


void NewHqlTransformer::analyseChildren(IHqlExpression * expr)
{
    ForEachChild(idx, expr)
        analyseExpr(expr->queryChild(idx));
}


void NewHqlTransformer::doAnalyseChildren(IHqlExpression * expr, unsigned first)
{
    unsigned max = expr->numChildren();
    for (unsigned idx = first; idx < max; idx++)
        analyseExpr(expr->queryChild(idx));
}



void NewHqlTransformer::doAnalyseChildren(IHqlExpression * expr, unsigned first, unsigned last)
{
    for (unsigned idx = first; idx < last; idx++)
        analyseExpr(expr->queryChild(idx));
}



void NewHqlTransformer::quickAnalyseTransform(IHqlExpression * expr)
{
    ForEachChild(i, expr)
    {
        IHqlExpression * cur = expr->queryChild(i);
        switch (cur->getOperator())
        {
        case no_assignall:
            quickAnalyseTransform(cur);
            break;
        case no_assign:
            if (quickTransformMustTraverse(cur->queryChild(1)))
                analyseAssign(cur);
            break;
        default:
            analyseExpr(cur);
            break;
        }
    }
}


void NewHqlTransformer::analyseExpr(IHqlExpression * expr)
{
#ifdef TRANSFORM_STATS
    stats.numAnalyseCalls++;
#endif

    node_operator op = expr->getOperator();
    switch (op)
    {
    case no_field:
    case no_record:
        break;
    case no_attr:
    case no_attr_expr:
    case no_attr_link:
    case no_constant:
    case no_translated:
    case no_getresult:
        break;
    case no_newkeyindex:
        //by default only look at the filename
        analyseExpr(expr->queryChild(3));
        break;
    case no_select:
        {
            IHqlExpression * ds = expr->queryChild(0);
#ifdef _DEBUG
            IHqlExpression * field = expr->queryChild(1);
#endif
            if (expr->hasProperty(newAtom))
                analyseExpr(ds);
            else
                analyseSelector(ds);
            break;
        }
    case no_filepos:
    case no_file_logicalname:
    case no_offsetof:
    case no_joined:
        analyseSelector(expr->queryChild(0));
        break;
    case no_activerow:
        analyseSelector(expr->queryChild(0));
        break;
    case no_assign:
        analyseAssign(expr);
        break;
    case no_newtransform:
    case no_transform:
        if (optimizeFlags & TCOtransformNonActive)
            quickAnalyseTransform(expr);
        else
            analyseChildren(expr);
        break;
    case no_sequential:
        {
            unsigned oldFlags = analyseFlags;
            analyseFlags |= TFsequential;
            analyseChildren(expr);
            analyseFlags = oldFlags;
            break;
        }
    case no_if:
        {
            analyseExpr(expr->queryChild(0));
            unsigned oldFlags = analyseFlags;
            analyseFlags = (analyseFlags & ~TFunconditional) | TFconditional;
            analyseExpr(expr->queryChild(1));
            if (expr->queryChild(2))
                analyseExpr(expr->queryChild(2));
            analyseFlags = oldFlags;
            break;
        }
    default:
        analyseChildren(expr);
        break;
    }
}

void NewHqlTransformer::analyseAssign(IHqlExpression * expr)
{
    //optimization: don't traverse LHS of an assignment
    analyseExpr(expr->queryChild(1));
}

void NewHqlTransformer::analyseSelector(IHqlExpression * expr)
{
    node_operator op = expr->getOperator();
    switch (op)
    {
    case no_selectnth:
        analyseExpr(expr);
        break;
    case no_select:
        if (expr->hasProperty(newAtom))
            analyseExpr(expr);
        else
            analyseSelector(expr->queryChild(0));
        break;
    }
}


IHqlExpression * NewHqlTransformer::quickTransformTransform(IHqlExpression * expr)
{
    bool same = false;
    unsigned max = expr->numChildren();
    HqlExprArray children;
    children.ensure(max);
    for (unsigned i=0; i < max; i++)
    {
        IHqlExpression * cur = expr->queryChild(i);
        IHqlExpression * tr;
        switch (cur->getOperator())
        {
        case no_assignall:
            tr = quickTransformTransform(cur);
            break;
        case no_assign:
            {
                IHqlExpression * src = cur->queryChild(1);
                if (quickTransformMustTraverse(src))
                    tr = transformAssign(cur);
                else
                {
#ifdef VERYIFY_OPTIMIZE
                    tr = transformAssign(cur);
                    assertex(tr == cur);
#else
                    tr = LINK(cur);
#endif
                }
                break;
            }
        default:
            tr = transform(cur);
            break;
        }
        children.append(*tr);
        if (cur != tr)
            same = false;
    }

    IHqlExpression * oldRecord = expr->queryRecord();
    if (oldRecord)
    {
        LinkedHqlExpr newRecord = queryTransformed(oldRecord);
        if (oldRecord != newRecord)
        {
            ITypeInfo * newRecordType = newRecord->queryRecordType();
            OwnedHqlExpr ret = createValue(expr->getOperator(), makeRowType(LINK(newRecordType)), children);
            return expr->cloneAllAnnotations(ret);
        }
    }

    if (same)
        return LINK(expr);
    return expr->clone(children);
}


//optimization: don't traverse LHS of an assignment, and minimise 
IHqlExpression * NewHqlTransformer::transformAssign(IHqlExpression * expr)
{
    IHqlExpression * rhs = expr->queryChild(1);
    OwnedHqlExpr newRhs = transform(rhs);
    if (rhs == newRhs)
        return LINK(expr);
    return createAssign(LINK(expr->queryChild(0)), newRhs.getClear());
}


/* In expr: not linked. return: linked */
IHqlExpression * NewHqlTransformer::createTransformed(IHqlExpression * expr)
{
    node_operator op = expr->getOperator();
    HqlExprArray children;
    bool same = true;
    switch (op)
    {
    case no_field:
    case no_record:
        return LINK(queryTransformed(expr));
    case no_cloned:
    case no_cachealias:
        break;
    case no_constant:
    case no_attr:
        return LINK(expr);
//  case no_attr_link:
//      return getTransformedChildren(expr);
    case no_newtransform:
        if (!expr->queryRecord())
            return completeTransform(expr, children);
        //fall through
    case no_transform:
        if ((op == no_transform) && (optimizeFlags & TCOtransformNonActive))
            return quickTransformTransform(expr);
        //fall through...
    case no_rowsetrange:
    case no_datasetlist:
    case no_rowset:
        //Another nasty: The type may have changed - but does not depend on any parameters...
        {
            same = transformChildren(expr, children);
            //NB: Check if type has changed after transforming children!
            ITypeInfo * type = expr->queryType();
            OwnedITypeInfo newType = transformType(type);
            if (type != newType)
            {
                OwnedHqlExpr ret = createValue(op, newType.getClear(), children);
                return expr->cloneAllAnnotations(ret);
            }
        }
        break;
    case no_select:
        if (expr->hasProperty(newAtom))
        {
            same = transformChildren(expr, children);
            IHqlExpression & ds = children.item(0);
            if (ds.getOperator() == no_activetable)
            {
                children.replace(*LINK(ds.queryChild(0)), 0);
                removeProperty(children, newAtom);
            }
        }
        else
            return createTransformedActiveSelect(expr);
        break;
    case no_filepos:
    case no_file_logicalname:
    case no_offsetof:
    case no_nameof:
        {
            IHqlExpression * selector = expr->queryChild(0);
            IHqlExpression * newSelector = transformSelector(selector);
            children.append(*newSelector);
            if (selector != newSelector)
                same = false;
            if (!transformChildren(expr, children))
                same = false;
            break;
        }
    case no_activerow:
        {
            IHqlExpression * ds = expr->queryChild(0);
            OwnedHqlExpr newDs = transformSelector(ds);
            if (ds != newDs)
            {
                if (newDs->getOperator() == no_newrow)
                    return LINK(newDs->queryChild(0));
                same = false;
                children.append(*newDs.getClear());
            }
            break;
        }
    case no_virtualscope:
        return transformScope(expr);
    case no_libraryscopeinstance:
        {
            IHqlExpression * oldFunction = expr->queryDefinition();
            OwnedHqlExpr newFunction = transform(oldFunction);
            HqlExprArray children;
            bool same = transformChildren(expr, children);
            if (!same || (newFunction != oldFunction))
                return createLibraryInstance(newFunction.getClear(), children);
            return LINK(expr);
        }
    case no_assign:
        return transformAssign(expr);
    case no_newkeyindex:
        {
            //Default action is to leave the (null) input dataset, record and transform untouched
            IHqlExpression * filename = expr->queryChild(3);
            OwnedHqlExpr newFilename = transform(filename);
            if (filename == newFilename)
                return LINK(expr);
            unwindChildren(children, expr);
            children.replace(*newFilename.getClear(), 3);
            return expr->clone(children);
        }
    case no_getgraphresult:
    case no_getgraphloopresult:
    case no_translated:
    case no_rows:
    case no_left:
    case no_right:
        return LINK(expr);
    case no_funcdef:
        {
            if (transformChildren(expr, children))
                return LINK(expr);
            return createFunctionDefinition(expr->queryName(), children);
        }
    default:
        return completeTransform(expr, children);
    }

    if (!same)
        return expr->clone(children);
    return LINK(expr);
}


IHqlExpression * NewHqlTransformer::transformSelector(IHqlExpression * expr)
{
#ifdef TRANSFORM_STATS
    stats.numTransformSelects++;
#endif

    expr = expr->queryNormalizedSelector();
    IHqlExpression * transformed = queryAlreadyTransformedSelector(expr);
    if (transformed)
    {
#ifdef TRANSFORM_STATS
        if (expr ==  transformed)
            stats.numTransformSelectsSame++;
#endif
        if (transformed->getOperator() == no_activerow)
            return LINK(transformed->queryChild(0));
        return LINK(transformed);
    }
    transformed = queryAlreadyTransformed(expr);
    if (transformed)
        transformed = LINK(transformed->queryNormalizedSelector());
    else
        transformed = createTransformedSelector(expr);
#ifdef TRANSFORM_STATS
    if (expr ==  transformed)
        stats.numTransformSelectsSame++;
#endif
    setTransformedSelector(expr, transformed);
    return transformed;
}

IHqlExpression * NewHqlTransformer::createTransformedSelector(IHqlExpression * expr)
{
    node_operator op = expr->getOperator();
    switch (op)
    {
//  case no_left:
//  case no_right:
//  case no_self:
    case no_selectnth:
        return transform(expr);
    case no_select:
        if (expr->hasProperty(newAtom))
            return transform(expr);
        return createTransformedActiveSelect(expr);
    default:
        //NB: no_if etc. should have gone down the transform branch, and then met a no_activerow if in scope.
        //NB: queryTransformed etc. have already been called.
        return LINK(expr->queryNormalizedSelector());
    }
}


IHqlExpression * NewHqlTransformer::createTransformedActiveSelect(IHqlExpression * expr)
{
    IHqlExpression * left = expr->queryChild(0);
    IHqlExpression * right = expr->queryChild(1);
    OwnedHqlExpr newLeft = transformSelector(left);
    IHqlExpression * newRight = right;                  // NB: Assumes fields don't change - unless transform for no_select overridden  [queryTransformed(right)]
    IHqlExpression * normLeft = left->queryNormalizedSelector();

    if ((normLeft == newLeft) && (newRight == right))
        return LINK(expr->queryNormalizedSelector());

    if (newLeft->getOperator() == no_newrow)
        return createNewSelectExpr(LINK(newLeft->queryChild(0)), LINK(newRight));

    if ((left->getOperator() == no_select) && left->isDatarow() && newLeft->isDatarow())
    {
        //weird situation x.a<new>.b   x.a<new> is converted to d, but d.b is not right, it should now be d.b<new>
        if ((newLeft->getOperator() != no_select) || (isNewSelector(left) && !isNewSelector(newLeft)))
            return createNewSelectExpr(LINK(newLeft), LINK(newRight));
    }

    //NOTE: In very obscure situations: ds[1].x.ds<new>.y -> z.ds<new>.y (newLeft != normalizeSelector)

    //NB: newLeft.get() == newLeft->queryNormalizedSelector() - asserted above
    return createSelectExpr(LINK(newLeft->queryNormalizedSelector()), LINK(newRight));
}


ANewTransformInfo * NewHqlTransformer::createTransformInfo(IHqlExpression * expr)
{
    return CREATE_NEWTRANSFORMINFO(NewTransformInfo, expr);
}


IHqlExpression * NewHqlTransformer::getTransformedChildren(IHqlExpression * expr)
{
    IHqlExpression * transformed = queryAlreadyTransformed(expr);
    if (transformed)
        return LINK(transformed);

    HqlExprArray children;
    unsigned max = expr->numChildren();
    unsigned idx;
    bool same = true;
    children.ensure(max);
    for (idx=0;idx<max;idx++)
    {
        IHqlExpression * child = expr->queryChild(idx);
        IHqlExpression * tchild = getTransformedChildren(child);
        children.append(*tchild);
        if (child != tchild)
            same = false;
    }

    //MORE: Need to have similar code to transform... to map the bodies.
    if (!same)
        transformed = expr->clone(children);
    else
        transformed = LINK(expr);

    //I'm not sure that this should really call setTransformed
    //worry about it later... - really should cache in a separate thing like transformedSelector
    setTransformed(expr, transformed);
    return transformed;
}


void NewHqlTransformer::initializeActiveSelector(IHqlExpression * expr, IHqlExpression * transformed)
{
    loop
    {
        setTransformedSelector(expr->queryNormalizedSelector(), transformed->queryNormalizedSelector());
        if (!expr->queryDataset())  // parent could be a row. e.g., LEFT.childDataset
            return;
        IHqlExpression * root = queryExpression(expr->queryDataset()->queryRootTable());
        if (!root || root->getOperator() != no_select)
            return;
        IHqlExpression * transformedRoot = queryExpression(transformed->queryDataset()->queryRootTable());
        assertex(transformedRoot);
        while (transformedRoot->getOperator() == no_alias)
            transformedRoot = queryExpression(transformedRoot->queryChild(0)->queryDataset()->queryRootTable());
        if (transformedRoot->getOperator() != no_select)
        {
            //What was a select has now become something else - e.g., if resourcing
            //may well cause dataset out of scope problems problems later.
            DBGLOG("Warning: a normalized select was replaced with a non-select.  This may cause unresolved datasets later");
            return;
        }
        expr = root->queryChild(0);
        transformed = transformedRoot->queryChild(0);
    }
}

IHqlExpression * NewHqlTransformer::queryTransformed(IHqlExpression * expr)
{
    IHqlExpression * transformed = queryAlreadyTransformed(expr);
    if (transformed)
        return transformed;
    return expr;
}

ANewTransformInfo * NewHqlTransformer::queryTransformExtra(IHqlExpression * expr)
{
    IInterface * extra = expr->queryTransformExtra();
    if (extra)
        return (NewTransformInfo *)extra;

    ANewTransformInfo * newExtra = createTransformInfo(expr);
    expr->setTransformExtraOwned(newExtra);
    return newExtra;
}


void NewHqlTransformer::stopDatasetTransform(IHqlExpression * expr)
{
    loop
    {
        IHqlExpression * prev;
        do
        {
            setTransformed(expr, expr);
            prev = expr;
            expr = expr->queryBody(true);
        } while (expr != prev);

        if (definesColumnList(expr))
            break;
        expr = expr->queryChild(0);
    }
}


void NewHqlTransformer::transformRoot(const HqlExprArray & in, HqlExprArray & out)
{
    ForEachItemIn(idx, in)
    {
        OwnedHqlExpr ret = doTransformRootExpr(&in.item(idx));
        if (ret)
            unwindCommaCompound(out, ret);
    }
}


IHqlExpression * NewHqlTransformer::doTransformRootExpr(IHqlExpression * expr)
{
    return transform(expr);
}


IHqlExpression * NewHqlTransformer::queryAlreadyTransformed(IHqlExpression * expr)
{
    IInterface * extra = expr->queryTransformExtra();
    if (extra)
        return ((NewTransformInfo *)extra)->queryTransformed();
    return NULL;
}

IHqlExpression * NewHqlTransformer::queryAlreadyTransformedSelector(IHqlExpression * expr)
{
    IInterface * extra = expr->queryTransformExtra();
    if (extra)
        return ((NewTransformInfo *)extra)->queryTransformedSelector();
    return NULL;
}

void NewHqlTransformer::setTransformed(IHqlExpression * expr, IHqlExpression * transformed)
{
#ifdef _DEBUG
    //Following test doesn't work because of call to setTransformed() in getTransformedChildren()
    //which doesn't seem a good idea - should revisit it.
    //assertex(!queryAlreadyTransformed(expr));
#endif
    queryTransformExtra(expr)->setTransformed(transformed);
}

void NewHqlTransformer::setTransformedSelector(IHqlExpression * expr, IHqlExpression * transformed)
{
    assertex(expr == expr->queryNormalizedSelector());
    //in rare situations a selector could get converted to a non-selector e.g, when replace self-ref with a new dataset.
    assertex(!transformed || transformed == transformed->queryNormalizedSelector() || transformed->hasProperty(newAtom));
    queryTransformExtra(expr)->setTransformedSelector(transformed);
}

/* In expr: not linked. Return: linked */
IHqlExpression * NewHqlTransformer::transform(IHqlExpression * expr)
{
#ifdef TRANSFORM_STATS
    stats.beginTransform();
#endif

    IHqlExpression * transformed = queryAlreadyTransformed(expr);
    if (transformed)
    {
#ifdef _DEBUG
        assertex(!(expr->isDatarow() && transformed->isDataset()));         // spot converting RIGHT to dataset without active row wrapper.
#endif
#ifdef TRANSFORM_STATS
        stats.endMatchTransform(expr, transformed);
#endif
        return LINK(transformed);
    }

#ifdef TRACK_ACTIVE_EXPRESSIONS
    activeExprStack.append(*expr);
#endif
    transformed = createTransformed(expr);
#ifdef TRACK_ACTIVE_EXPRESSIONS
    activeExprStack.pop();
#endif

#ifdef TRANSFORM_STATS
    stats.endNewTransform(expr, transformed);
#endif

#ifdef _DEBUG
    assertex(!(expr->isDatarow() && transformed->isDataset()));
#endif
    setTransformed(expr, transformed);

    //Need to map the bodies of this expression to the corresponding children, otherwise
    //the type of a transform doesn't change, even though the named record has.
    IHqlExpression * body = expr->queryBody(true);
    if (body != expr)
    {
        IHqlExpression * transformedBody = transformed->queryBody(true);
        do
        {
            //If child body is already mapped then don't remap it, otherwise tree can become inconsistent
            if (queryAlreadyTransformed(body))
                break;
            setTransformed(body, transformedBody);
            expr = body;
            body = body->queryBody(true);
            transformedBody = transformedBody->queryBody(true);
        } while (expr != body);
    }

    return transformed;
}


IHqlExpression * NewHqlTransformer::queryTransformAnnotation(IHqlExpression * expr)
{
    IHqlExpression * body = expr->queryBody(true);
    if (expr == body)
        return NULL;
    return createTransformedAnnotation(expr);
}


IHqlExpression * NewHqlTransformer::createTransformedAnnotation(IHqlExpression * expr)
{
    annotate_kind kind = expr->getAnnotationKind();
    IHqlExpression * body = expr->queryBody(true);

    if (kind == annotate_symbol || kind == annotate_location)
        locations.append(*expr);
    OwnedHqlExpr newBody = transform(body);
    if (kind == annotate_symbol || kind == annotate_location)
        locations.pop();
    if (body == newBody)
        return LINK(expr);
    if (hasNamedSymbol(newBody) && expr->isNamedSymbol())
        return newBody.getClear();
    return expr->cloneAnnotation(newBody);
}

IHqlExpression * NewHqlTransformer::queryActiveSymbol() const
{
    ForEachItemInRev(i, locations)
    {
        IHqlExpression & cur = locations.item(i);
        if (cur.getAnnotationKind() == annotate_symbol)
            return &cur;
    }
    return NULL;
}

IHqlExpression * NewHqlTransformer::queryActiveLocation() const
{
    if (locations.ordinality() == 0)
        return NULL;
    return &locations.tos();
}

IHqlExpression * NewHqlTransformer::queryActiveLocation(IHqlExpression * expr) const
{
    if (expr)
    {
        IHqlExpression * location = queryLocation(expr);
        if (location)
            return location;
    }
    return queryActiveLocation();
}

IHqlExpression * NewHqlTransformer::transformExternalCall(IHqlExpression * expr)
{
    IHqlExpression * body = expr->queryBody();
    IHqlExpression * oldFuncdef = body->queryExternalDefinition();
    OwnedHqlExpr newFuncdef = transform(oldFuncdef);
    
    HqlExprArray args;
    bool same = transformChildren(body, args);
    if (same && (oldFuncdef == newFuncdef))
        return LINK(expr);
    OwnedHqlExpr newCall = CHqlExternalCall::makeExternalCall(LINK(newFuncdef), newFuncdef->queryChild(0)->getType(), args);
    return expr->cloneAllAnnotations(newCall);
}


IHqlExpression * NewHqlTransformer::transformCall(IHqlExpression * expr)
{
    IHqlExpression * body = expr->queryBody();
    IHqlExpression * oldFuncdef = body->queryFunctionDefinition();
    OwnedHqlExpr newFuncdef = transform(oldFuncdef);
    
    HqlExprArray args;
    bool same = transformChildren(body, args);
    if (same && (oldFuncdef == newFuncdef))
        return LINK(expr);
    OwnedHqlExpr newCall = createBoundFunction(NULL, newFuncdef, args, NULL, DEFAULT_EXPAND_CALL);
    return expr->cloneAllAnnotations(newCall);
}


void NewHqlTransformer::setMapping(IHqlExpression * oldValue, IHqlExpression * newValue)
{
    setSelectorMapping(oldValue, newValue);
    setMappingOnly(oldValue, newValue);
}

void NewHqlTransformer::setMappingOnly(IHqlExpression * oldValue, IHqlExpression * newValue)
{
    loop
    {
        setTransformed(oldValue, newValue);
        IHqlExpression * body = oldValue->queryBody(true);
        if (oldValue == body)
            break;
        oldValue = body;
        newValue = newValue->queryBody(true);
    }
}

void NewHqlTransformer::setSelectorMapping(IHqlExpression * oldValue, IHqlExpression * newValue)
{
    setTransformedSelector(oldValue->queryNormalizedSelector(), newValue->queryNormalizedSelector());
}

IHqlExpression * NewHqlTransformer::doUpdateOrphanedSelectors(IHqlExpression * expr, IHqlExpression * transformed)
{
    //If the parent has changed to then the active selector may have changed out of step with the parent dataset
    //so need to explicitly remap the dataset.  
    //Happens in constant folding when filters etc. are replaced with a very different child
    //Happens when also hoisting a non-table expression e.g, globalAutoHoist

    IHqlExpression * newDs = transformed->queryChild(0);
    if (newDs && newDs->isDataset())
    {
        //More: a specialised HEF flag would make this more efficient...
        childDatasetType childType = getChildDatasetType(expr);
        switch (childType)
        {
        case childdataset_dataset:
        case childdataset_datasetleft: 
        case childdataset_top_left_right:
            {
                IHqlExpression * ds = expr->queryChild(0);
                if (newDs != ds)
                {
                    OwnedHqlExpr transformedSelector = transformSelector(ds->queryNormalizedSelector());
                    IHqlExpression * newSelector = newDs->queryNormalizedSelector();
                    if (transformedSelector != newSelector)
                    {
                        //DBGLOG("****Mismatched selector for %s->%s****", getOpString(ds->getOperator()), getOpString(newDs->getOperator()));
                        HqlExprArray args;
                        args.append(*LINK(newDs));
                        replaceSelectors(args, transformed, 1, transformedSelector, newSelector);
                        return transformed->clone(args);
                    }
                }
                break;
            }
        }
    }
    return LINK(transformed);
}

//---------------------------------------------------------------------------

static HqlTransformerInfo hqlMapTransformerInfo("HqlMapTransformer");
HqlMapTransformer::HqlMapTransformer() : NewHqlTransformer(hqlMapTransformerInfo)
{
}

IHqlExpression * HqlMapTransformer::queryMapping(IHqlExpression * oldValue)
{
    return queryTransformed(oldValue);
}

IHqlExpression * replaceExpression(IHqlExpression * expr, IHqlExpression * original, IHqlExpression * replacement)
{
    if (expr == original)
        return LINK(replacement);
    HqlMapTransformer simpleTransformer;
    simpleTransformer.setMapping(original, replacement);
    return simpleTransformer.transformRoot(expr);
}


HqlMapSelectorTransformer::HqlMapSelectorTransformer(IHqlExpression * oldDataset, IHqlExpression * newDataset)
{
    oldSelector.set(oldDataset);
    IHqlExpression * newSelector = newDataset;
    if (newDataset->getOperator() == no_newrow)
        newDataset = newDataset->queryChild(0);

    node_operator op = oldDataset->getOperator();
#ifndef ENSURE_SELSEQ_UID
    assertex(op != no_left && op != no_right);
#endif
    if (oldDataset->isDatarow() || op == no_activetable || op == no_self || op == no_selfref)
    {
        setMappingOnly(oldDataset, newDataset);
    }
    else
    {
        setMappingOnly(oldDataset, oldDataset);         // Don't change any new references to the dataset
    }
    setSelectorMapping(oldDataset, newSelector);
}


static HqlTransformerInfo hqlScopedMapSelectorTransformerInfo("HqlScopedMapSelectorTransformer");
HqlScopedMapSelectorTransformer::HqlScopedMapSelectorTransformer(IHqlExpression * oldDataset, IHqlExpression * newDataset)
: MergingHqlTransformer(hqlScopedMapSelectorTransformerInfo)
{
    IHqlExpression * newSelector = newDataset;
    if (newDataset->getOperator() == no_newrow)
        newDataset = newDataset->queryChild(0);

    node_operator op = oldDataset->getOperator();
    if (oldDataset->isDatarow() || op == no_activetable || op == no_self || op == no_selfref)
    {
        setMappingOnly(oldDataset, newDataset);         // A row, so Don't change any new references to the dataset
        //MORE: We should be very 
    }
    else
    {
        setMappingOnly(oldDataset, oldDataset);         // Don't change any new references to the dataset
    }
    setSelectorMapping(oldDataset, newSelector);

}


static HqlTransformerInfo nestedHqlMapTransformerInfo("NestedHqlMapTransformer");
NestedHqlMapTransformer::NestedHqlMapTransformer() : NestedHqlTransformer(nestedHqlMapTransformerInfo)
{
}


//---------------------------------------------------------------------------

HoistingHqlTransformer::HoistingHqlTransformer(HqlTransformerInfo & _info, unsigned _flags) : NewHqlTransformer(_info)
{ 
    flags = _flags; 
    target = NULL; 
    conditionDepth = 0; 
    containsUnknownIndependentContents = false;
}

void HoistingHqlTransformer::transformRoot(const HqlExprArray & in, HqlExprArray & out)
{
    HqlExprArray * savedTarget = target;
    target = &out;

    //If there is a single ensureresult, then transform it specially, so that any hoisted values are
    //only inside the ensure result.  If multiple assume we want to create cses globally (e.g., shared stored)
    if ((in.ordinality() == 1) && (in.item(0).getOperator() == no_ensureresult))
        out.append(*transformEnsureResult(&in.item(0)));
    else
        NewHqlTransformer::transformRoot(in, out);
    target = savedTarget;
}


void HoistingHqlTransformer::appendToTarget(IHqlExpression & cur)
{
    target->append(cur);
}

ANewTransformInfo * HoistingHqlTransformer::createTransformInfo(IHqlExpression * expr)
{
    return CREATE_NEWTRANSFORMINFO(HoistingTransformInfo, expr);
}

void HoistingHqlTransformer::transformArray(const HqlExprArray & in, HqlExprArray & out)
{
    HqlExprArray * savedTarget = target;
    target = &out;
    ForEachItemIn(idx, in)
        out.append(*transform(&in.item(idx)));
    target = savedTarget;
}

IHqlExpression * HoistingHqlTransformer::transformRoot(IHqlExpression * expr)
{
    HqlExprArray args, transformed;
    unwindCommaCompound(args, expr);
    transformRoot(args, transformed);
    return createActionList(transformed);
}


bool HoistingHqlTransformer::analyseThis(IHqlExpression * expr)
{
    HoistingTransformInfo * extra = queryExtra(expr->queryBody());
    if (alreadyVisited(expr))
    {
        if ((pass != 0) || extra->isUnconditional() || (conditionDepth > 0))
            return false;
    }
    if (conditionDepth == 0)
        extra->setUnconditional();
    return true;
}


void HoistingHqlTransformer::analyseExpr(IHqlExpression * expr)
{
    if (!analyseThis(expr))
        return;

    doAnalyseExpr(expr);
}


void HoistingHqlTransformer::doAnalyseExpr(IHqlExpression * expr)
{
    node_operator op = expr->getOperator();
    switch (op)
    {
    case no_colon:
        //Hoisting transforms need to happen after the workflow separation - otherwise
        //you can end up with an exponential expansion of persist points.
        throwUnexpected();              
    case no_cluster:
    case no_sequential:
        containsUnknownIndependentContents = true;
        return;
    case no_allnodes:
    case no_thisnode:
        if (!(flags & HTFtraverseallnodes))
            return;
        break;
    case no_globalscope:
        if (!expr->hasProperty(localAtom))
        {
            unsigned savedDepth = conditionDepth;
            conditionDepth = 0;
            analyseExpr(expr->queryChild(0));
            conditionDepth = savedDepth;
            return;
        }
        break;
    case no_if:
        if (checkConditional(expr))
        {
            analyseExpr(expr->queryChild(0));
            conditionDepth++;
            analyseExpr(expr->queryChild(1));
            if (expr->queryChild(2))
                analyseExpr(expr->queryChild(2));
            conditionDepth--;
            return;
        }
        break;
    case no_attr_expr:
        analyseChildren(expr);
        return;
    }
    NewHqlTransformer::analyseExpr(expr);
}

//A good idea, but I need to get my head around roxie/thor differences and see if we can execute graphs more dynamically.
IHqlExpression * HoistingHqlTransformer::createTransformed(IHqlExpression * expr)
{
//#ifdef _DEBUG
//  assertex(expr->queryBody() == expr);
//#endif

    node_operator op = expr->getOperator();
    switch (op)
    {
    case no_allnodes:               // MORE: This needs really needs to recurse, and substitute within no_thisnode - not quite sure how that would happen
    case no_thisnode:
        //I'm not sure this is a good solution - really contents of no_thisnode should be hoisted in common, would require dependence on insideAllNodes
        if (!(flags & HTFtraverseallnodes))
            return LINK(expr);
        break;
    case no_colon:
    case no_cluster:
    case no_sequential:
        {
            HqlExprArray args;
            ForEachChild(i, expr)
                args.append(*transformIndependent(expr->queryChild(i)));
            return expr->clone(args);
        }
    case no_apply:
        {
            //Temporary fix for bug #30255
            OwnedHqlExpr transformed = NewHqlTransformer::createTransformed(expr);
            IHqlExpression * child = expr->queryChild(0);
            IHqlExpression * transformedChild = transformed->queryChild(0);
            if (child->queryNormalizedSelector() != transformedChild->queryNormalizedSelector())
            {
                HqlExprArray args;
                args.append(*LINK(transformedChild));
                ForEachChildFrom(i, transformed, 1)
                    args.append(*replaceSelector(transformed->queryChild(i), child->queryNormalizedSelector(), transformedChild->queryNormalizedSelector()));
                return transformed->clone(args);
            }
            return transformed.getClear();
        }
    case no_subgraph:
        {
            HqlExprArray args, transformedArgs;
            unwindChildren(args, expr);
            transformArray(args, transformedArgs);
            return completeTransform(expr, transformedArgs);
        }
    case no_if:
        if (checkConditional(expr))
        {
            HqlExprArray args;
            args.append(*transform(expr->queryChild(0)));
            {
                OwnedHqlExpr transformedTrue = transform(expr->queryChild(1));
                args.append(*transformIndependent(transformedTrue));
            }
            if (queryRealChild(expr, 2))
            {
                OwnedHqlExpr transformedFalse = transform(expr->queryChild(2));
                args.append(*transformIndependent(transformedFalse));
            }
            return completeTransform(expr, args);
        }
        break;
    }
    return NewHqlTransformer::createTransformed(expr);
}

IHqlExpression * HoistingHqlTransformer::transformEnsureResult(IHqlExpression * expr)
{
    OwnedHqlExpr transformed = transformRoot(expr->queryChild(0));
    HqlExprArray args;
    unwindCommaCompound(args, transformed);
    unsigned max = args.ordinality();
    OwnedHqlExpr value;
    if (!args.item(max-1).isAction())
    {
        value.setown(&args.popGet());
        if (args.ordinality())
        {
            OwnedHqlExpr actions = createActionList(args);
            value.setown(createCompound(actions.getClear(), LINK(value)));
        }
    }
    else
        value.setown(createActionList(args));

    return replaceChild(expr, 0, value);
}

//---------------------------------------------------------------------------

void NestedHqlTransformer::beginNestedScope()
{
    depthStack.append(savedSelectors.ordinality());
    depthStack.append(savedTransformed.ordinality());
}

void NestedHqlTransformer::endNestedScope()
{
    unsigned prevTransforms = depthStack.pop();
    unsigned prevSelectors = depthStack.pop();

    while (savedSelectors.ordinality() != prevSelectors)
    {
        IHqlExpression & cur = savedSelectors.pop();
        NewHqlTransformer::setTransformedSelector(&cur, NULL);
    }

    while (savedTransformed.ordinality() != prevTransforms)
    {
        OwnedHqlExpr prev = &savedTransformedValue.popGet();
        if (prev == savedNull)
            prev.clear();
        IHqlExpression & cur = savedTransformed.pop();
        NewHqlTransformer::setTransformed(&cur, prev);
    }
}

void NestedHqlTransformer::setTransformed(IHqlExpression * expr, IHqlExpression * transformed)
{
    if (depthStack.ordinality() != 0)
    {
        savedTransformed.append(*expr);
        IHqlExpression * saved = queryTransformed(expr);
        if (!saved) saved = savedNull;
        savedTransformedValue.append(*LINK(saved));
    }
    NewHqlTransformer::setTransformed(expr, transformed);
}

void NestedHqlTransformer::setTransformedSelector(IHqlExpression * expr, IHqlExpression * transformed)
{
    if (depthStack.ordinality() != 0)
        savedSelectors.append(*expr);
    NewHqlTransformer::setTransformedSelector(expr, transformed);
}


//---------------------------------------------------------------------------

bool onlyTransformOnce(IHqlExpression * expr)
{
    switch (expr->getOperator())
    {
    case no_field:
    case no_constant:
    case no_record:
    case no_param:
    case no_translated:
    case no_attr:
    case no_all:
    case no_counter:
    case no_sequence:
    case no_self:
    case no_selfref:
#ifdef ENSURE_SELSEQ_UID
    case no_left:
    case no_right:
#endif
    case no_flat:
    case no_any:
    case no_existsgroup:
    case no_countgroup:
        return true;
    case no_pat_const:
        //Many more patterns could go here.  Would be safer if I had a flag to indicate they contained validators/transforms.
        return true;

    case no_skip:
    case no_thor:
    case no_csv:
    case no_xml:
    case no_list:
        return (expr->numChildren() == 0);
    case no_select:
        if (expr->queryChild(0)->getOperator() == no_self)
            return true;
        break;
    case no_assign:
        //return onlyTransformOnce(expr->queryChild(1));
        if (onlyTransformOnce(expr->queryChild(1)))
            return true;
        break;
    default:
//      is this valid?
//      if (original->isConstant())
//          onlyTransformOnce = true;
        break;
    }
    return false;
}

//---------------------------------------------------------------------------

MergingTransformSimpleInfo::MergingTransformSimpleInfo(IHqlExpression * _expr) : AMergingTransformInfo(_expr) 
{ 
}

IHqlExpression * MergingTransformSimpleInfo::queryAlreadyTransformed(IHqlExpression * childScope)
{
    return inlineQueryTransformed();
}


void MergingTransformSimpleInfo::setTransformed(IHqlExpression * childScope, IHqlExpression * value)
{
    inlineSetTransformed(value);
}


IHqlExpression * MergingTransformSimpleInfo::queryAlreadyTransformedSelector(IHqlExpression * childScope)
{
    return inlineQueryTransformedSelector();
}


void MergingTransformSimpleInfo::setTransformedSelector(IHqlExpression * childScope, IHqlExpression * value)
{
    inlineSetTransformedSelector(value);
}


#if 0
//---------------------------------------------------------------------------

MergingTransformComplexCache::MergingTransformComplexCache()
{ 
    mapped = NULL; 
    mappedSelector = NULL;
}

IHqlExpression * MergingTransformComplexCache::queryAlreadyTransformed(AMergingTransformInfo & baseInfo, IHqlExpression * childScope)
{
    IHqlExpression * curTransformed = baseInfo.queryTransformed();
    if (!curTransformed)
        return NULL;
    if (transformedScope == childScope || onlyTransformOnce())
        return curTransformed;
    if (!mapped)
        return NULL;
    OwnedHqlExpr * matched = mapped->getValue(childScope);
    if (matched)
    {
        transformedScope.set(childScope);
        IHqlExpression * prevTransformed = matched->get();
        baseInfo.setTransformed(prevTransformed, childScope);
        return prevTransformed;
    }
    return NULL;
}


void MergingTransformComplexCache::setTransformed(AMergingTransformInfo & baseInfo, IHqlExpression * childScope, IHqlExpression * value)
{
    IHqlExpression * curTransformed = baseInfo.queryTransformed();
    if (curTransformed)
    {
        assertex(!onlyTransformOnce());
        if (!mapped)
        {
            mapped = new MapOwnedToOwned<IHqlExpression, IHqlExpression>;
            mapped->setValue(transformedScope, curTransformed);
        }
        mapped->setValue(childScope, value);
    }
    transformedScope.set(childScope);
    baseInfo.setTransformed(value, childScope);
}


IHqlExpression * MergingTransformComplexCache::queryAlreadyTransformedSelector(AMergingTransformInfo & baseInfo, IHqlExpression * childScope)
{
    IHqlExpression * curTransformedSelector = baseInfo.queryTransformedSelector();
    if (!curTransformedSelector)
        return NULL;
    if (transformedSelectorScope == childScope || onlyTransformOnce())
        return curTransformedSelector;

    if (!mappedSelector)
    {
        if (!baseInfo.recurseParentScopes())
            return NULL;
        IHqlExpression * curTransformed = queryAlreadyTransformed(baseInfo, childScope);
        if (curTransformed)
            return curTransformed->queryNormalizedSelector();

        while (childScope)
        {
            if (childScope->getOperator() == no_comma)
                childScope = childScope->queryChild(1);
            else
                childScope = NULL;
            if (transformedSelectorScope == childScope)
                return curTransformedSelector;
        }
        return NULL;
    }
    OwnedHqlExpr * matched = mappedSelector->getValue(childScope);
    if (matched)
    {
        transformedSelectorScope.set(childScope);
        IHqlExpression * prevTransformedSelector = matched->get();
        baseInfo.setTransformedSelector(prevTransformedSelector, childScope);
        return prevTransformedSelector;
    }

    if (childScope)
    {
        if (!baseInfo.recurseParentScopes())
            return NULL;

        //This is quite similar in intent to the code in initializeActiveSelector, which really acts as an
        //optimization by inserting translations from a previous scope into the current scope.
        //However it doesn't always do enough - e.g., when converting a dataset to NULL, hence the need for the
        //full search.  See sqaggds3 for and example that requires it.
        IHqlExpression * curTransformed = queryAlreadyTransformed(baseInfo, childScope);
        if (curTransformed)
            return curTransformed->queryNormalizedSelector();

        while (childScope)
        {
            if (childScope->getOperator() == no_comma)
                childScope = childScope->queryChild(1);
            else
                childScope = NULL;
            OwnedHqlExpr * matched = mappedSelector->getValue(childScope);
            if (matched)
                return matched->get();
            if (mapped)
            {
                matched = mapped->getValue(childScope);
                if (matched)
                    return matched->get();
            }
        }
    }
    return NULL;
}


void MergingTransformComplexCache::setTransformedSelector(AMergingTransformInfo & baseInfo, IHqlExpression * childScope, IHqlExpression * value)
{
    IHqlExpression * curTransformedSelector = baseInfo.queryTransformedSelector();
    if (curTransformedSelector)
    {
        if (onlyTransformOnce())
        {
            assertex(value == curTransformedSelector);
            return;
        }
        if (!mappedSelector)
        {
            mappedSelector = new MapOwnedToOwned<IHqlExpression, IHqlExpression>;
            mappedSelector->setValue(transformedSelectorScope, curTransformedSelector);
        }
        mappedSelector->setValue(childScope, value);
    }
    transformedSelectorScope.set(childScope);
    baseInfo.setTransformedSelector(value, childScope);
}
#endif

//---------------------------------------------------------------------------

MergingTransformInfo::MergingTransformInfo(IHqlExpression * _expr) : AMergingTransformInfo(_expr) 
{ 
    mapped = NULL; 
    mappedSelector = NULL;
    setOnlyTransformOnce(::onlyTransformOnce(original));
}

IHqlExpression * MergingTransformInfo::queryAlreadyTransformed(IHqlExpression * childScope)
{
    IHqlExpression * curTransformed = inlineQueryTransformed();
    if (!curTransformed)
        return NULL;
    if (transformedScope == childScope || onlyTransformOnce())
        return curTransformed;
    if (!mapped)
        return NULL;
    IHqlExpression * matched = mapped->getValue(childScope);
    if (matched)
    {
        transformedScope.set(childScope);
        IHqlExpression * prevTransformed = matched;
        inlineSetTransformed(prevTransformed);
        return prevTransformed;
    }
    return NULL;
}


void MergingTransformInfo::setTransformed(IHqlExpression * childScope, IHqlExpression * value)
{
    IHqlExpression * curTransformed = inlineQueryTransformed();
    if (curTransformed)
    {
        assertex(!onlyTransformOnce());
        if (!mapped)
        {
            mapped = new MAPPINGCLASS;
            mapped->setValue(transformedScope, curTransformed);
        }
        mapped->setValue(childScope, value);
#if 0
        if (mapped->map.count() > maxMapCount)
            maxMapCount  = mapped->map.count();
        static HqlExprArray seen;
        if (mapped->map.count() >= 84 && !seen.contains(*original))
        {
            seen.append(*LINK(original));
            DBGLOG(getOpString(original->getOperator()));
//          dbglogExpr(original);
            void * cur = NULL;
            while ((cur = mapped->map.next(cur)) != NULL)
            {
                MappingOwnedToOwned<IHqlExpression,IHqlExpression> * x = (MappingOwnedToOwned<IHqlExpression,IHqlExpression>*)(IInterface *)cur;
                Owned<IHqlExpression> * scope = (Owned<IHqlExpression>*)x->getKey();
                StringBuffer txt;
                HqlExprArray z;
                if (*scope)
                    (*scope)->unwindList(z, no_comma);
                ForEachItemIn(i, z)
                    txt.append(" ").append(getOpString(z.item(i).getOperator()));
                DBGLOG("@%s", txt.str());
//              dbglogExpr(*scope);
                x = NULL;
            }
            cur = NULL;
        }
#endif
    }
    transformedScope.set(childScope);
    inlineSetTransformed(value);
}


IHqlExpression * MergingTransformInfo::queryAlreadyTransformedSelector(IHqlExpression * childScope)
{
    IHqlExpression * curTransformedSelector = inlineQueryTransformedSelector();
    if (!curTransformedSelector)
        return NULL;
    if (transformedSelectorScope == childScope || onlyTransformOnce())
        return curTransformedSelector;

    if (!mappedSelector)
    {
        if (!recurseParentScopes())
            return NULL;
        IHqlExpression * curTransformed = queryAlreadyTransformed(childScope);
        if (curTransformed)
            return curTransformed->queryNormalizedSelector();

        while (childScope)
        {
            if (childScope->getOperator() == no_comma)
                childScope = childScope->queryChild(1);
            else
                childScope = NULL;
            if (transformedSelectorScope == childScope)
                return curTransformedSelector;
        }
        return NULL;
    }
    IHqlExpression * matched = mappedSelector->getValue(childScope);
    if (matched)
    {
        transformedSelectorScope.set(childScope);
        IHqlExpression * prevTransformedSelector = matched;
        inlineSetTransformedSelector(prevTransformedSelector);
        return prevTransformedSelector;
    }

    if (childScope)
    {
        if (!recurseParentScopes())
            return NULL;

        //This is quite similar in intent to the code in initializeActiveSelector, which really acts as an
        //optimization by inserting translations from a previous scope into the current scope.
        //However it doesn't always do enough - e.g., when converting a dataset to NULL, hence the need for the
        //full search.  See sqaggds3 for and example that requires it.
        IHqlExpression * curTransformed = queryAlreadyTransformed(childScope);
        if (curTransformed)
            return curTransformed->queryNormalizedSelector();

        while (childScope)
        {
            if (childScope->getOperator() == no_comma)
                childScope = childScope->queryChild(1);
            else
                childScope = NULL;
            IHqlExpression * matched = mappedSelector->getValue(childScope);
            if (matched)
                return matched;
            if (mapped)
            {
                matched = mapped->getValue(childScope);
                if (matched)
                    return matched;
            }
        }
    }
    return NULL;
}


void MergingTransformInfo::setTransformedSelector(IHqlExpression * childScope, IHqlExpression * value)
{
    IHqlExpression * curTransformedSelector = inlineQueryTransformedSelector();
    if (curTransformedSelector)
    {
        if (onlyTransformOnce())
        {
            assertex(value == curTransformedSelector);
            return;
        }

        if (!mappedSelector)
        {
            mappedSelector = new MAPPINGCLASS;
            mappedSelector->setValue(transformedSelectorScope, curTransformedSelector);
        }
        mappedSelector->setValue(childScope, value);
    }
    transformedSelectorScope.set(childScope);
    inlineSetTransformedSelector(value);
}

//---------------------------------------------------------------------------

IHqlExpression * MergingHqlTransformer::createTransformed(IHqlExpression * expr)
{
    node_operator op = expr->getOperator();
    switch (op)
    {
    case no_activerow:
        return NewHqlTransformer::createTransformed(expr);
    case no_select:
        if (!expr->hasProperty(newAtom))
            return createTransformedActiveSelect(expr);
        break;
    }

    switch (getChildDatasetType(expr))
    {
    case childdataset_dataset: 
    case childdataset_datasetleft: 
    case childdataset_top_left_right:
        {
            IHqlExpression * arg0 = expr->queryChild(0);
            OwnedHqlExpr child = transform(arg0);

            HqlExprArray children;
            children.append(*LINK(child));
            pushChildContext(arg0, child);
            bool same = optimizedTransformChildren(expr, children);
            popChildContext();
            if (!same)
                return expr->clone(children);
            return LINK(expr);
        }
    case childdataset_merge:
        {
            unsigned firstAttr = getNumChildTables(expr);
            IHqlExpression * arg0 = expr->queryChild(0);
            OwnedHqlExpr child = transform(arg0);
            HqlExprArray children;
            children.append(*LINK(child));
            for (unsigned i=1; i < firstAttr; i++)
                children.append(*transform(expr->queryChild(i)));
            pushChildContext(arg0, child);
            bool same = optimizedTransformChildren(expr, children);
            popChildContext();
            if (!same)
                return expr->clone(children);
            return LINK(expr);
        }
    case childdataset_evaluate:
        throwUnexpected();
        {
            // I doubt this really works, but by the time any sensible transform is applied to the tree 
            // evaluates will have been converted to projects instead
            IHqlExpression * arg0 = expr->queryChild(0);
            OwnedHqlExpr child = transform(arg0);

            HqlExprArray children;
            children.append(*LINK(child));
            pushChildContext(arg0->queryNormalizedSelector(true), child->queryNormalizedSelector(true));
            bool same = optimizedTransformChildren(expr, children);
            popChildContext();
            if (!same)
                return expr->clone(children);
            return LINK(expr);
        }
    default:
        return NewHqlTransformer::createTransformed(expr);
    }
}

ANewTransformInfo * MergingHqlTransformer::createTransformInfo(IHqlExpression * expr)
{
    if (onlyTransformOnce(expr))
        return CREATE_NEWTRANSFORMINFO(MergingTransformSimpleInfo, expr);
    return CREATE_NEWTRANSFORMINFO(MergingTransformInfo, expr);
}

void MergingHqlTransformer::pushChildContext(IHqlExpression * expr, IHqlExpression * transformed)
{
    assert(expr->getOperator() != no_comma);
    IHqlExpression * scope = expr;
    //NB: Do no call createComma because that calls createDataset which unwinds a comma list!
    if (childScope)
        childScope.setown(createValue(no_comma,LINK(scope), childScope.getClear()));
    else
        childScope.set(scope);
    initializeActiveSelector(expr, transformed);
}

void MergingHqlTransformer::popChildContext()
{
    if (childScope->getOperator() == no_comma)
        childScope.set(childScope->queryChild(1));
    else
        childScope.clear();
}


IHqlExpression * MergingHqlTransformer::queryAlreadyTransformed(IHqlExpression * expr)
{
    AMergingTransformInfo * extra = queryExtra(expr);
    return extra->queryAlreadyTransformed(childScope);
}


void MergingHqlTransformer::setTransformed(IHqlExpression * expr, IHqlExpression * transformed)
{
    AMergingTransformInfo * extra = queryExtra(expr);
    extra->setTransformed(childScope, transformed);
}


IHqlExpression * MergingHqlTransformer::queryAlreadyTransformedSelector(IHqlExpression * expr)
{
    AMergingTransformInfo * extra = queryExtra(expr->queryNormalizedSelector());
    return extra->queryAlreadyTransformedSelector(childScope);
}


void MergingHqlTransformer::setTransformedSelector(IHqlExpression * expr, IHqlExpression * transformed)
{
    assertex(expr == expr->queryNormalizedSelector());
    assertex(transformed == transformed->queryNormalizedSelector());
    AMergingTransformInfo * extra = queryExtra(expr->queryNormalizedSelector());
    extra->setTransformedSelector(childScope, transformed);
}

//---------------------------------------------------------------------------

#if 0
SelectorReplacingTransformer::SelectorReplacingTransformer() 
{ 
    introducesAmbiguity = false; 
    savedNewDataset = NULL; 
    isHidden = false; 
}

void SelectorReplacingTransformer::initSelectorMapping(IHqlExpression * _oldDataset, IHqlExpression * _newDataset)
{
    oldDataset.set(_oldDataset);
    newSelector.set(_newDataset);
    if (_newDataset->getOperator() == no_newrow)
        newDataset.set(_newDataset->queryChild(0));
    else
        newDataset.set(_newDataset);

    if (newDataset->getOperator() == no_activerow)
        savedNewDataset = newDataset->queryChild(0)->queryNormalizedSelector();

    node_operator op = oldDataset->getOperator();
    if (op == no_left || op == no_right)
        oldSelector.set(oldDataset);

    updateMapping();
}


void SelectorReplacingTransformer::updateMapping()
{
    node_operator op = oldDataset->getOperator();
    if (oldDataset->isDatarow() || op == no_activetable || op == no_self || op == no_selfref)
    {
        if (isAlwaysActiveRow(newDataset) || newDataset->isDatarow())
            setMappingOnly(oldDataset, newDataset);         // A row, so Don't change any new references to the dataset
        else
        {
            OwnedHqlExpr newActive = ensureActiveRow(newDataset);
            setMappingOnly(oldDataset, newActive);          // A row, so Don't change any new references to the dataset
        }
    }
    else
    {
        setMappingOnly(oldDataset, oldDataset);         // Don't change any new references to the dataset
    }
    setSelectorMapping(oldDataset, newSelector);
}

IHqlExpression * SelectorReplacingTransformer::createTransformed(IHqlExpression * expr)
{
    if (expr->queryNormalizedSelector() == savedNewDataset)
        introducesAmbiguity = true;

    unsigned numNonHidden = activityHidesSelectorGetNumNonHidden(expr, oldSelector);
    if (numNonHidden == 0)
        return MergingHqlTransformer::createTransformed(expr);

    LinkedHqlExpr oldChildContext = oldSelector.get();
    LinkedHqlExpr newChildContext = oldSelector.get();
    HqlExprArray children;
    switch (getChildDatasetType(expr))
    {
    case childdataset_dataset: 
        throwUnexpected();
    case childdataset_datasetleft: 
    case childdataset_top_left_right:
        {
            assertex(numNonHidden == 1);
            IHqlExpression * arg0 = expr->queryChild(0);
            OwnedHqlExpr child = transform(arg0);
            oldChildContext.set(arg0);
            newChildContext.set(child);
            children.append(*LINK(child));
            break;
        }
    default:
        {
            for (unsigned i=0; i < numNonHidden; i++)
                children.append(*transform(expr->queryChild(i)));
            break;
        }
    }


    bool wasHidden = isHidden;
    isHidden = true;
    pushChildContext(oldChildContext, newChildContext);
    bool same = transformChildren(expr, children);
    popChildContext();
    isHidden = wasHidden;
    if (!same)
        return expr->clone(children);
    return LINK(expr);
}


void SelectorReplacingTransformer::pushChildContext(IHqlExpression * expr, IHqlExpression * transformed)
{
    MergingHqlTransformer::pushChildContext(expr, transformed);
    if (isHidden)
        setTransformedSelector(oldSelector, oldSelector);
    else
        updateMapping();
}
#endif

//---------------------------------------------------------------------------

static HqlTransformerInfo newSelectorReplacingTransformerInfo("NewSelectorReplacingTransformer");
NewSelectorReplacingTransformer::NewSelectorReplacingTransformer() : NewHqlTransformer(newSelectorReplacingTransformerInfo)
{ 
    introducesAmbiguity = false; 
    savedNewDataset = NULL; 
    isHidden=false; 
}


void NewSelectorReplacingTransformer::initSelectorMapping(IHqlExpression * oldDataset, IHqlExpression * newDataset)
{
    IHqlExpression * newSelector = newDataset;
    if (newDataset->getOperator() == no_newrow)
        newDataset = newDataset->queryChild(0);

    if (newDataset->getOperator() == no_activerow)
        savedNewDataset = newDataset->queryChild(0)->queryNormalizedSelector();

    node_operator op = oldDataset->getOperator();
    if (oldDataset->isDatarow() || op == no_activetable || op == no_self || op == no_selfref)
    {
        if (isAlwaysActiveRow(newDataset) || newDataset->isDatarow())
            setMappingOnly(oldDataset, newDataset);         // A row, so Don't change any new references to the dataset
        else
        {
            OwnedHqlExpr newActive = ensureActiveRow(newDataset);
            setMappingOnly(oldDataset, newActive);          // A row, so Don't change any new references to the dataset
        }
    }
    else
    {
        setMappingOnly(oldDataset, oldDataset);         // Don't change any new references to the dataset
    }
    setSelectorMapping(oldDataset, newSelector);

    if (op == no_left || op == no_right)
        oldSelector.set(oldDataset);
}


IHqlExpression * NewSelectorReplacingTransformer::createTransformed(IHqlExpression * expr)
{
    if (!isHidden && expr->queryNormalizedSelector() == savedNewDataset)
        introducesAmbiguity = true;

    unsigned numNonHidden = activityHidesSelectorGetNumNonHidden(expr, oldSelector);
    if (numNonHidden == 0)
        return NewHqlTransformer::createTransformed(expr);

    HqlExprArray children;
    for (unsigned i=0; i < numNonHidden; i++)
        children.append(*transform(expr->queryChild(i)));

    bool wasHidden = isHidden;
    isHidden = true;
    bool same = optimizedTransformChildren(expr, children);
    isHidden = wasHidden;
    if (!same)
        return expr->clone(children);
    return LINK(expr);
}

IHqlExpression * NewSelectorReplacingTransformer::queryAlreadyTransformedSelector(IHqlExpression * expr)
{
    NewSelectorReplacingInfo * extra = queryExtra(expr);
    return extra->queryTransformedSelector(isHidden);
}


IHqlExpression * newReplaceSelector(IHqlExpression * expr, IHqlExpression * oldSelector, IHqlExpression * newSelector)
{
    NewSelectorReplacingTransformer transformer;
    transformer.initSelectorMapping(oldSelector, newSelector);
    OwnedHqlExpr ret = transformer.transformRoot(expr);
    if (transformer.foundAmbiguity())
        DBGLOG("Mapping introduces potential ambiguity into expression");
    return ret.getClear();
}

void newReplaceSelector(HqlExprArray & target, const HqlExprArray & source, IHqlExpression * oldSelector, IHqlExpression * newSelector)
{
    NewSelectorReplacingTransformer transformer;
    transformer.initSelectorMapping(oldSelector, newSelector);
    ForEachItemIn(i, source)
        target.append(*transformer.transformRoot(&source.item(i)));
    if (transformer.foundAmbiguity())
        DBGLOG("Mapping introduces potential ambiguity into expression");
}

IHqlExpression * queryNewReplaceSelector(IHqlExpression * expr, IHqlExpression * oldSelector, IHqlExpression * newSelector)
{
    NewSelectorReplacingTransformer transformer;
    transformer.initSelectorMapping(oldSelector, newSelector);
    OwnedHqlExpr ret = transformer.transformRoot(expr);
    if (transformer.foundAmbiguity())
        return NULL;
    return ret.getClear();
}

//---------------------------------------------------------------------------

/*
 Scoping following is tricky....
 Need to know following:
 o What scopes are surrounding me?
   - a list of activity scopes is maintained.
 o Am in the process of creating a new dataset?
   - examples that create a new dataset are sum(ds) output(x)
   - selectnth creates a new scope for its first child, but not the others.
   - evaluate(x) is a bit strange - it creates a temporary scope, but doesn't create a new dataset.
   - x := y where y is a dataset needs to be handled specially. (Similarly for compare/order)

 So:
 A new scope is created whenever something is introducing a new line of scopes.
 Each operator sets the scope for it's children, and then clears it at the end.
 */

IHqlDataset * ScopeInfo::queryActiveDataset()
{
    if (dataset)
        return dataset->queryDataset()->queryRootTable();
    return NULL;
}


//---------------------------------------------------------------------------

ScopedTransformer::ScopedTransformer(HqlTransformerInfo & _info) : NewHqlTransformer(_info)
{
    innerScope = NULL;
    emptyScopeMarker.setown(createAttribute(scopeAtom));
}


void ScopedTransformer::analyseAssign(IHqlExpression * expr)
{
    //optimization: don't traverse LHS of an assignment
    IHqlExpression * left = expr->queryChild(0);
    IHqlExpression * right = expr->queryChild(1);
    if (left->isDataset() || left->isDatarow())
    {
        pushScope();
        analyseExpr(right);
        popScope();
    }
    else
    {
        analyseExpr(right);
    }
}

void ScopedTransformer::analyseChildren(IHqlExpression * expr)
{
    node_operator op = expr->getOperator();
    unsigned idx;

    switch (op)
    {
    case no_joined:     // A couple of weird exceptions....
        analyseSelector(expr->queryChild(0));
        break;
    case no_sizeof:
    case no_offsetof:
    case no_nameof:
    case no_updatestate:
        if (!expr->queryChild(0)->isDataset())
        {
            NewHqlTransformer::analyseChildren(expr);
            return;
        }
        //fallthrough
    case NO_AGGREGATE:
    case no_countfile:
    case no_countindex:
    case no_buildindex:
    case no_apply:
    case no_distributer:
    case no_distribution:
    case no_within:
    case no_notwithin:
    case no_output:
    case no_writespill:
    case no_createset:
    case no_soapaction_ds:
    case no_newsoapaction_ds:
    case no_returnresult:
    case no_setgraphresult:
    case no_setgraphloopresult:
    case no_extractresult:
        {
            IHqlExpression * dataset = expr->queryChild(0);
            pushScope();
            if ((op == no_within) || (op == no_notwithin))
                innerScope->isWithin = true;
            analyseExpr(dataset);
            bool nested = setDataset(dataset, dataset);
            unsigned numChildren = expr->numChildren();
            for (idx = 1; idx < numChildren; idx++)
                analyseExpr(expr->queryChild(idx));
            clearDataset(nested);
            popScope();
            break;
        }
    case no_projectrow:
        {
            //Ugly - should probably try and remove these from the parse tree
            IHqlExpression * dataset = expr->queryChild(0);
            pushScope();
            analyseExpr(dataset);
            bool nested = setLeft(dataset, querySelSeq(expr));
            unsigned numChildren = expr->numChildren();
            for (idx = 1; idx < numChildren; idx++)
                analyseExpr(expr->queryChild(idx));
            clearDataset(nested);
            popScope();
            break;
        }
    case no_keydiff:
    case no_rowdiff:
        {
            pushScope();
            analyseExpr(expr->queryChild(0));
            analyseExpr(expr->queryChild(1));
            popScope();
            unsigned numChildren = expr->numChildren();
            for (idx = 2; idx < numChildren; idx++)
                analyseExpr(expr->queryChild(idx));
            break;
        }
    case no_setresult:
    case no_blob2id:
        {
            IHqlExpression * value = expr->queryChild(0);
            unsigned first = 0;
            if (value->isDataset())
            {
                pushScope();
                analyseExpr(value);
                popScope();
                first++;
            }
            unsigned numChildren = expr->numChildren();
            for (idx=first; idx < numChildren; idx++)
                analyseExpr(expr->queryChild(idx));
            break;
        }
    case no_keypatch:
    case no_selectnth:
        {
            pushScope();
            analyseExpr(expr->queryChild(0));
            popScope();
            unsigned numChildren = expr->numChildren();
            for (idx = 1; idx < numChildren; idx++)
                analyseExpr(expr->queryChild(idx));
            break;
        }
    case no_evaluate:
        {
            IHqlExpression * scope = expr->queryChild(0);
            analyseExpr(scope);
            pushEvaluateScope(scope, scope);
            analyseExpr(expr->queryChild(1));
            popEvaluateScope();
            break;
        }
    case no_assign:
        analyseAssign(expr);
        break;
    case no_lt:
    case no_le:
    case no_gt:
    case no_ge:
    case no_ne:
    case no_eq:
    case no_order:
        {
            IHqlExpression * left = expr->queryChild(0);
            IHqlExpression * right = expr->queryChild(1);
            if (left->isDataset() || left->isDatarow())
            {
                pushScope();
                analyseExpr(left);
                analyseExpr(right);
                popScope();
            }
            else
            {
                analyseExpr(left);
                analyseExpr(right);
            }
            break;
        }
    case no_keyed:
    case no_loopbody:
        {
            pushScope();
            ForEachChild(idx, expr)
                analyseExpr(expr->queryChild(idx));
            popScope();
            break;
        }
    case no_compound:
    case no_mapto:
        {
            if (innerScope && innerScope->isEmpty())
            {
                suspendScope();
                analyseExpr(expr->queryChild(0));
                restoreScope();
            }
            else
                analyseExpr(expr->queryChild(0));
            analyseExpr(expr->queryChild(1));
            break;
        }
    case no_table:
        {
            unsigned max = expr->numChildren();
            unsigned idx = 0;
            suspendScope();
            for (; idx < 3; idx++)
                analyseExpr(expr->queryChild(idx));
            restoreScope();
            if (max >= 4)
            {
                IHqlExpression * ds = expr->queryChild(idx++);
                analyseExpr(ds);
                bool nested = setDataset(ds, ds);
                for (; idx < max; idx++)
                    analyseExpr(expr->queryChild(idx));
                clearDataset(nested);
            }
            break;
        }
    case no_select:
        {
            if (expr->hasProperty(newAtom))
            {
                IHqlExpression * dataset = expr->queryChild(0);
                pushScope();
                analyseExpr(dataset);
                bool nested = setDataset(dataset, dataset);
                analyseExpr(expr->queryChild(1));
                clearDataset(nested);
                popScope();
            }
            else
                NewHqlTransformer::analyseChildren(expr);
            break;
        }
    case no_globalscope:
        if (expr->hasProperty(optAtom))
        {
            NewHqlTransformer::analyseChildren(expr);
            break;
        }
        //fall through
    case no_colon:
    case no_cluster:
        {
            // By definition no_colon is evaluated globally, so no tables are in scope for its children
            ScopeSuspendInfo info;
            suspendAllScopes(info);

            if (expr->isDataset())
                pushScope();
            analyseExpr(expr->queryChild(0));
            if (expr->isDataset())
                popScope();
            unsigned numChildren = expr->numChildren();
            for (idx = 1; idx < numChildren; idx++)
                analyseExpr(expr->queryChild(idx));

            restoreScopes(info);
            break;
        }
    case no_keyindex:
    case no_newkeyindex:
        {
            IHqlExpression * dataset = expr->queryChild(0);
            analyseSelector(dataset);
            bool nested = setDataset(dataset, dataset);
            unsigned max = expr->numChildren();
            for (unsigned idx=1; idx < max; idx++)
                analyseExpr(expr->queryChild(idx));
            clearDataset(nested);
            break;
        }
    case no_activerow:
        analyseSelector(expr->queryChild(0));
        break;
    case no_attr_expr:
    case no_call:
    case no_externalcall:
    case no_rowvalue:
    case no_setmeta:
    case no_typetransfer:
    case no_subgraph:
    case no_libraryscopeinstance:
        {
            if (expr->isDataset())
                suspendScope();
            unsigned max = expr->numChildren();
            for (unsigned idx=0; idx < max; idx++)
            {
                IHqlExpression * cur = expr->queryChild(idx);
                if (cur->isDataset())
                    pushScope();
                analyseExpr(cur);
                if (cur->isDataset())
                    popScope();
            }
            if (expr->isDataset())
                restoreScope();
            break;
        }
    default:
        {
            unsigned numChildren = expr->numChildren();
            unsigned first = (unsigned) -1;
            bool nested = false;
            switch (getChildDatasetType(expr))
            {
            case childdataset_none:
            case childdataset_addfiles:
            case childdataset_map:
            case childdataset_dataset_noscope:
                {
                    //Don't change scope.
                    NewHqlTransformer::analyseChildren(expr);
                    return;
                }
            case childdataset_merge:
                {
                    first = getNumChildTables(expr);
                    for (idx = 0; idx < first; idx++)
                        analyseExpr(expr->queryChild(idx));
                    IHqlExpression * dataset = expr->queryChild(0);
                    nested = setDataset(dataset, dataset);
                    break;
                }
            case childdataset_if:
            case childdataset_case:
                {
                    assertex(innerScope && innerScope->isEmpty());
                    suspendScope();
                    analyseExpr(expr->queryChild(0));
                    restoreScope();
                    for (idx = 1; idx < numChildren; idx++)
                        analyseExpr(expr->queryChild(idx));
                    return;
                }
            case childdataset_dataset:
                {
                    IHqlExpression * dataset = expr->queryChild(0);
                    analyseExpr(dataset);
                    nested = setDataset(dataset, dataset);
                    first = 1;
                }
                break;
            case childdataset_datasetleft:
                {
                    IHqlExpression * dataset = expr->queryChild(0);
                    analyseExpr(dataset);
                    nested = setDatasetLeft(dataset, dataset, querySelSeq(expr));
                    first = 1;
                }
                break;
            case childdataset_left:
                {
                    IHqlExpression * left = expr->queryChild(0);
                    analyseExpr(left);
                    nested = setLeft(left, querySelSeq(expr));
                    first = 1;
                }
                break;
            case childdataset_leftright:
                {
                    IHqlExpression * left = expr->queryChild(0);
                    IHqlExpression * right = expr->queryChild(1);
                    IHqlExpression * selSeq = querySelSeq(expr);
                    analyseExpr(left);
                    if (expr->getOperator() == no_normalize)                // right can be dependent on left
                    {
                        bool nested = setLeft(left, selSeq);
                        pushScope();
                        analyseExpr(right);
                        popScope();
                        clearDataset(nested);
                    }
                    else
                        analyseExpr(right);
                    nested = setLeftRight(left, right, selSeq);
                    first = 2;
                }
                break;
            case childdataset_top_left_right:
                {
                    IHqlExpression * left = expr->queryChild(0);
                    analyseExpr(left);
                    nested = setTopLeftRight(left, left, querySelSeq(expr));
                    first = 1;
                }
                break;
            case childdataset_same_left_right:
            case childdataset_nway_left_right:              
                {
                    IHqlExpression * left = expr->queryChild(0);
                    analyseExpr(left);
                    nested = setLeftRight(left, left, querySelSeq(expr));
                    first = 1;
                    if (op == no_selfjoin)
                        first = 2;
                }
                break;
            case childdataset_evaluate:
                //done above...
            default:
                UNIMPLEMENTED;
            }

            for (idx = first; idx < numChildren; idx++)
                analyseExpr(expr->queryChild(idx));
            clearDataset(nested);
            break;
        }
    }
}


IHqlExpression * ScopedTransformer::createTransformed(IHqlExpression * expr)
{
    HqlExprArray children;

    node_operator op = expr->getOperator();
    unsigned numChildren = expr->numChildren();
    children.ensure(numChildren);

    unsigned idx;
    switch (op)
    {
    case no_constant:
        return LINK(expr);
    case no_sizeof:
    case no_offsetof:
    case no_nameof:
//      if (!expr->queryChild(0)->isDataset())
            return NewHqlTransformer::createTransformed(expr);
        throwUnexpected();
    case no_updatestate:
        if (!expr->queryChild(0)->isDataset())
            return NewHqlTransformer::createTransformed(expr);
        //fallthrough
    case NO_AGGREGATE:
    case no_joined:
    case no_countfile:
    case no_countindex:
    case no_buildindex:
    case no_apply:
    case no_distribution:
    case no_distributer:
    case no_within:
    case no_notwithin:
    case no_output:
    case no_createset:
    case no_soapaction_ds:
    case no_newsoapaction_ds:
    case no_returnresult:
    case no_setgraphresult:
    case no_setgraphloopresult:
    case no_extractresult:
        {
            IHqlExpression * dataset = expr->queryChild(0);
            pushScope();
            IHqlExpression * transformedDs = transform(dataset);
            children.append(*transformedDs);
            bool nested = setDataset(dataset, transformedDs);
            for (idx = 1; idx < numChildren; idx++)
                children.append(*transform(expr->queryChild(idx)));
            clearDataset(nested);
            popScope();
            break;
        }
    case no_projectrow:
        {
            //ugly - see comment above
            IHqlExpression * dataset = expr->queryChild(0);
            pushScope();
            IHqlExpression * transformedDs = transform(dataset);
            children.append(*transformedDs);
            bool nested = setLeft(dataset, querySelSeq(expr));
            for (idx = 1; idx < numChildren; idx++)
                children.append(*transform(expr->queryChild(idx)));
            clearDataset(nested);
            popScope();
            break;
        }
    case no_keydiff:
    case no_rowdiff:
        {
            pushScope();
            children.append(*transform(expr->queryChild(0)));
            children.append(*transform(expr->queryChild(1)));
            for (idx = 2; idx < numChildren; idx++)
                children.append(*transform(expr->queryChild(idx)));
            popScope();
            break;
        }
    case no_setresult:
    case no_blob2id:
        {
            IHqlExpression * value = expr->queryChild(0);
            unsigned first = 0;
            if (value->isDataset())
            {
                pushScope();
                children.append(*transform(value));
                popScope();
                first++;
            }
            for (idx=first; idx < numChildren; idx++)
                children.append(*transform(expr->queryChild(idx)));
            break;
        }
    case no_selectnth:
    case no_keypatch:
        {
            pushScope();
            children.append(*transform(expr->queryChild(0)));
            popScope();
            for (idx = 1; idx < numChildren; idx++)
                children.append(*transform(expr->queryChild(idx)));
            break;
        }
    case no_evaluate:
        {
            IHqlExpression * scope = expr->queryChild(0);
            IHqlExpression * transformedScope = transform(scope);
            children.append(*transformedScope);
            pushEvaluateScope(scope, transformedScope);
            children.append(*transform(expr->queryChild(1)));
            popEvaluateScope();
            break;
        }
    case no_assign:
        {
            IHqlExpression * left = expr->queryChild(0);
            IHqlExpression * right = expr->queryChild(1);
            children.append(*LINK(left));
            if (left->isDataset() || left->isDatarow())
            {
                pushScope();
                children.append(*transform(right));
                popScope();
            }
            else
            {
                children.append(*transform(right));
            }
            break;
        }
    case no_lt:
    case no_le:
    case no_gt:
    case no_ge:
    case no_ne:
    case no_eq:
    case no_order:
        {
            IHqlExpression * left = expr->queryChild(0);
            IHqlExpression * right = expr->queryChild(1);
            if (left->isDataset() || left->isDatarow())
            {
                pushScope();
                children.append(*transform(left));
                children.append(*transform(right));
                popScope();
            }
            else
            {
                children.append(*transform(left));
                children.append(*transform(right));
            }
            break;
        }
    case no_keyed:
    case no_loopbody:
        {
            pushScope();
            ForEachChild(idx, expr)
                children.append(*transform(expr->queryChild(idx)));
            popScope();
            break;
        }
    case no_mapto:
    case no_compound:
        {
            if (innerScope && innerScope->isEmpty())
            {
                suspendScope();
                children.append(*transform(expr->queryChild(0)));
                restoreScope();
            }
            else
                children.append(*transform(expr->queryChild(0)));
            children.append(*transform(expr->queryChild(1)));
            break;
        }
    case no_table:
        {
            suspendScope();
            bool restored=false;
            bool nested = false;
            ForEachChild(idx, expr)
            {
                IHqlExpression * cur = expr->queryChild(idx);
                if (idx == 3 && cur->isDataset())
                {
                    restored = true;
                    restoreScope();
                    IHqlExpression * transformed = transform(cur);
                    nested = setDataset(cur, transformed);
                    children.append(*transformed);
                }
                else
                    children.append(*transform(cur));
            }
            if (restored)
                clearDataset(nested);
            else
                restoreScope();
            break;
        }
    case no_select:
        {
            IHqlExpression * dataset = expr->queryChild(0);
            if (expr->hasProperty(newAtom))
            {
                pushScope();
                IHqlExpression * transformedDs = transform(dataset);
                children.append(*transformedDs);
                bool nested = setDataset(dataset, transformedDs);
                for (idx = 1; idx < numChildren; idx++)
                    children.append(*transform(expr->queryChild(idx)));
                clearDataset(nested);
                popScope();
                if (transformedDs->getOperator() == no_activetable)
                {
                    children.replace(*LINK(transformedDs->queryChild(0)), 0);
                    removeProperty(children, newAtom);
                }
            }
            else
                return NewHqlTransformer::createTransformed(expr);
            break;
        }
    case no_globalscope:
        if (expr->hasProperty(optAtom))
            return NewHqlTransformer::createTransformed(expr);
        //fall through
    case no_colon:
    case no_cluster:
        {
            // By definition no_colon is evaluated globally, so no tables are in scope for its children
            ScopeSuspendInfo info;
            suspendAllScopes(info);

            if (expr->isDataset())
                pushScope();
                children.append(*transform(expr->queryChild(0)));
            if (expr->isDataset())
                popScope();
            for (idx = 1; idx < numChildren; idx++)
                children.append(*transform(expr->queryChild(idx)));

            restoreScopes(info);
            break;
        }
    case no_keyindex:
    case no_newkeyindex:
        {
            IHqlExpression * dataset = expr->queryChild(0);
            IHqlExpression * transformedDs = transform(dataset);
            children.append(*transformedDs);
            bool nested = setDataset(dataset, transformedDs);
            unsigned max = expr->numChildren();
            for (unsigned idx=1; idx < max; idx++)
                children.append(*transform(expr->queryChild(idx)));
            clearDataset(nested);
            break;
        }
    case no_activerow:
        {
            IHqlExpression * dataset = expr->queryChild(0);
            IHqlExpression * transformedDs = transformSelector(dataset);
            children.append(*transformedDs);
            break;
        }
    case no_call:
    case no_externalcall:
    case no_attr_expr:
    case no_rowvalue:
    case no_setmeta:
    case no_typetransfer:
    case no_subgraph:
    case no_libraryscopeinstance:
        {
            if (expr->isDataset())
                suspendScope();
            unsigned max = expr->numChildren();
            for (unsigned idx=0; idx < max; idx++)
            {
                IHqlExpression * cur = expr->queryChild(idx);
                if (cur->isDataset())
                    pushScope();
                children.append(*transform(cur));
                if (cur->isDataset())
                    popScope();
            }
            if (expr->isDataset())
                restoreScope();
            break;
        }
    default:
        {
            unsigned first = (unsigned) -1;
            bool nested = false;
            switch (getChildDatasetType(expr))
            {
            case childdataset_none:
            case childdataset_addfiles:
            case childdataset_map:
            case childdataset_dataset_noscope:
                {
                    return NewHqlTransformer::createTransformed(expr);
                }
            case childdataset_merge:
                {
                    first = getNumChildTables(expr);
                    IHqlExpression * dataset = expr->queryChild(0);
                    IHqlExpression * transformedDs = transform(dataset);
                    children.append(*transformedDs);
                    for (idx = 1; idx < first; idx++)
                        children.append(*transform(expr->queryChild(idx)));
                    nested = setDataset(dataset, transformedDs);
                    break;
                }
            case childdataset_if:
            case childdataset_case:
                {
                    assertex(innerScope && innerScope->isEmpty());
                    suspendScope();
                    children.append(*transform(expr->queryChild(0)));
                    restoreScope();
                    for (idx = 1; idx < numChildren; idx++)
                        children.append(*transform(expr->queryChild(idx)));
                    return expr->clone(children);
                }
            case childdataset_dataset:
                {
                    IHqlExpression * dataset = expr->queryChild(0);
                    IHqlExpression * transformedDs = transform(dataset);
                    children.append(*transformedDs);
                    nested = setDataset(dataset, transformedDs);
                    first = 1;
                }
                break;
            case childdataset_datasetleft:
                {
                    IHqlExpression * dataset = expr->queryChild(0);
                    IHqlExpression * transformedDs = transform(dataset);
                    children.append(*transformedDs);
                    nested = setDatasetLeft(dataset, transformedDs, querySelSeq(expr));
                    first = 1;
                }
                break;
            case childdataset_left:
                {
                    IHqlExpression * left = expr->queryChild(0);
                    children.append(*transform(left));
                    nested = setLeft(left, querySelSeq(expr));
                    first = 1;
                }
                break;
            case childdataset_leftright:
                {
                    IHqlExpression * left = expr->queryChild(0);
                    IHqlExpression * right = expr->queryChild(1);
                    children.append(*transform(left));
                    children.append(*transform(right));
                    nested = setLeftRight(left, right, querySelSeq(expr));
                    first = 2;
                }
                break;
            case childdataset_top_left_right:
                {
                    IHqlExpression * left = expr->queryChild(0);
                    IHqlExpression * transformedLeft = transform(left);
                    children.append(*transformedLeft);
                    nested = setTopLeftRight(left, transformedLeft, querySelSeq(expr));
                    first = 1;
                }
                break;
            case childdataset_same_left_right:
            case childdataset_nway_left_right:              
                {
                    IHqlExpression * left = expr->queryChild(0);
                    children.append(*transform(left));
                    if (op == no_selfjoin)
                        children.append(*transform(expr->queryChild(1)));
                    nested = setLeftRight(left, left, querySelSeq(expr));
                    first = children.ordinality();
                }
                break;
            case childdataset_evaluate:
                //done above...
            default:
                UNIMPLEMENTED;
            }

            for (idx = first; idx < numChildren; idx++)
                children.append(*transform(expr->queryChild(idx)));
            clearDataset(nested);
            break;
        }
    }
    return cloneOrLink(expr, children);
}


IHqlExpression * ScopedTransformer::getEvaluateScope(IHqlExpression * scope)
{
    switch (scope->getOperator())
    {
    case no_left:
        return innerScope->left.getLink();
    case no_right:
        return innerScope->right.getLink();
    default:
        //MORE: We need to remove the LEFT/RIGHT from the scope (if present), and then apply the
        //remaining selections to the result.
        //Something like...
        //OwnedHqlExpr scope1 = replaceSelector(scope, queryPseudoTable(no_left), innerScope->left);
        //OwnedHqlExpr scope2 = replaceSelector(scope, queryPseudoTable(no_right), innerScope->right);
        return LINK(scope);
    }
}


void ScopedTransformer::pushScope()
{
    innerScope = new ScopeInfo;
    scopeStack.append(*innerScope);
}

void ScopedTransformer::pushEvaluateScope(IHqlExpression * dataset, IHqlExpression * transformed)
{
    innerScope = new ScopeInfo;
    scopeStack.append(*innerScope);

    innerScope->setDataset(dataset->queryNormalizedSelector(true), transformed);
    //MORE: Need to correctly translate the dataset....  Not sure what we want to do really.
//  innerScope->evaluateScope.setown(getEvaluateScope(dataset));
}

void ScopedTransformer::popScope()
{
    scopeStack.pop();
    if (scopeStack.ordinality())
        innerScope = &scopeStack.tos();
    else
        innerScope = NULL;
}

void ScopedTransformer::popEvaluateScope()
{
    popScope();
}

void ScopedTransformer::clearDataset(bool nested)                                       
{
    assertex(innerScope->dataset || innerScope->left);
    innerScope->clear();
}

bool ScopedTransformer::isDatasetRelatedToScope(IHqlExpression * search)
{
    if (search->getOperator() == no_selectnth)
        search = search->queryChild(0);
    if (search->queryDataset())
    {
        ForEachItemInRev(idx, scopeStack)
        {
            ScopeInfo & cur = scopeStack.item(idx);
            if (cur.dataset)
            {
                if (isChildRelationOf(search, cur.dataset))
                    return true;
            }
#if 1
            //Removed for the moment, because I think this only needs to work for hole tables
            if (cur.left)
            {
                OwnedHqlExpr left = createSelector(no_left, cur.left, cur.seq);
                if (isChildRelationOf(search, left))
                    return true;
            }
#endif
        }
    }
    else if (search->getOperator() == no_select)
    {
        //field in a nested record, test the selector
        return isDatasetRelatedToScope(search->queryChild(0));
    }
    switch (search->getOperator())
    {
    case no_left:
    case no_right:
    case no_matchattr:
        return true;
    case no_if:
        return isDatasetRelatedToScope(search->queryChild(1)) || isDatasetRelatedToScope(search->queryChild(2));
    }
    return false;
}

bool ScopedTransformer::checkInScope(IHqlExpression * selector, bool allowCreate)
{
    switch (selector->getOperator())
    {
    case no_left:
    case no_right:
    case no_self:
    case no_selfref:
    case no_activetable:
    case no_matchattr:
        return true;    
    case no_field:
        return true;    
        // assume the expression couldn't have been parsed without this being true.....
        //otherwise we need to search up the list checking if a scope exists that defines left/right.
    case no_selectnth:
        //Indexing any dataset is almost certainly ok - except for child dataset of out of scope dataset
        //don't bother checking.
        return allowCreate;
    case no_select:
        if (selector->isDataset())
            break;
        return checkInScope(selector->queryChild(0), allowCreate);
    case no_globalscope:
        if (selector->hasProperty(optAtom))
            break;
        //fall through
    case no_colon:
    case no_cluster:
        // By definition no_colon is evaluated globally, so it can always be accessed
        if (allowCreate && selector->isDatarow())
            return true;
        break;
    }

    if (scopeStack.ordinality() == 0)
        return false;

    IHqlExpression * normalized = selector->queryNormalizedSelector(false);
    ForEachItemInRev(idx, scopeStack)
    {
        ScopeInfo & cur = scopeStack.item(idx);
        if (cur.dataset && cur.dataset->queryNormalizedSelector(false) == normalized)
            return true;

        if (isInImplictScope(cur.dataset, normalized))
            return true;
    }
    return false;
}

bool ScopedTransformer::isDatasetActive(IHqlExpression * selector)
{
    return checkInScope(selector, false);
}

bool ScopedTransformer::isDatasetARow(IHqlExpression * selector)
{
    return checkInScope(selector, true);
}

bool ScopedTransformer::isTopDataset(IHqlExpression * selector)
{
    if (scopeStack.ordinality())
    {
        ScopeInfo & top = scopeStack.tos();
        if (top.dataset && top.dataset->queryNormalizedSelector(false) == selector->queryNormalizedSelector(false))
            return true;
        switch (selector->getOperator())
        {
        case no_left:
            return (top.left != NULL);
        case no_right:
            return (top.right != NULL);
        case no_select:
            if (selector->isDataset())
                return false;
            return isTopDataset(selector->queryChild(0));
        }
    }
    return false;
}


IHqlExpression * ScopedTransformer::getScopeState()
{
    HqlExprArray attrs;
    ForEachItemIn(idx, scopeStack)
    {
        ScopeInfo & cur = scopeStack.item(idx);
        if (cur.dataset)
            attrs.append(*LINK(cur.dataset));
        else if (cur.left)
            attrs.append(*createAttribute(leftAtom, LINK(cur.left), LINK(cur.right)));
        else
            {}/*No need to include this*/
    }
    unsigned num = attrs.ordinality();
    if (num == 0)
        return LINK(emptyScopeMarker);
    if (num == 1) 
        return &attrs.popGet();                     
    return createAttribute(scopeAtom, attrs);
}

void ScopedTransformer::suspendAllScopes(ScopeSuspendInfo & info)
{
    ForEachItemIn(i1, scopeStack)
        info.scope.append(scopeStack.item(i1));
    ForEachItemIn(i2, savedStack)
        info.saved.append(savedStack.item(i2));
    scopeStack.kill(true);
    savedStack.kill(true);
    innerScope = NULL;
}

void ScopedTransformer::suspendScope()
{
    assertex(innerScope->isEmpty());
    savedStack.append(*LINK(innerScope));
    popScope();
}


void ScopedTransformer::restoreScope()
{
    innerScope = &savedStack.popGet();
    scopeStack.append(*innerScope);
}

void ScopedTransformer::restoreScopes(ScopeSuspendInfo & info)
{
    ForEachItemIn(i1, info.scope)
        scopeStack.append(info.scope.item(i1));
    ForEachItemIn(i2, info.saved)
        savedStack.append(info.saved.item(i2));
    info.scope.kill(true);
    info.saved.kill(true);
    if (scopeStack.ordinality())
        innerScope = &scopeStack.tos();
}

unsigned ScopedTransformer::tableNesting()
{
    unsigned numTables = scopeStack.ordinality();
    if (isNewDataset())
        numTables--;
    return numTables;
}

bool ScopedTransformer::insideActivity()
{
    return (scopeStack.ordinality() != 0) || (savedStack.ordinality() != 0);
}

IHqlExpression * ScopedTransformer::doTransformRootExpr(IHqlExpression * expr)
{
    IHqlExpression * ret;
    if (expr->isDataset())
    {
        pushScope();
        ret = NewHqlTransformer::doTransformRootExpr(expr);
        popScope();
    }
    else
        ret = NewHqlTransformer::doTransformRootExpr(expr);
    assertex(scopeStack.ordinality() == 0);
    return ret;
}


void ScopedTransformer::throwScopeError()
{
    throwError(HQLERR_DatasetNotExpected);
}


//---------------------------------------------------------------------------

ScopedDependentTransformer::ScopedDependentTransformer(HqlTransformerInfo & _info) : ScopedTransformer(_info)
{
    cachedLeft.setown(createValue(no_left));
    cachedRight.setown(createValue(no_right));
}

bool ScopedDependentTransformer::setDataset(IHqlExpression * ds, IHqlExpression * transformedDs)
{
    ScopedTransformer::setDataset(ds, transformedDs);
    pushChildContext(ds, transformedDs);
    return true;
}


bool ScopedDependentTransformer::setDatasetLeft(IHqlExpression * ds, IHqlExpression * transformedDs, IHqlExpression * seq)
{
    ScopedTransformer::setDatasetLeft(ds, transformedDs, seq);
    pushChildContext(ds, transformedDs);
    return true;
}


bool ScopedDependentTransformer::setLeft(IHqlExpression * _left, IHqlExpression * seq)
{
    ScopedTransformer::setLeft(_left, seq);
    return false;
    pushChildContext(cachedLeft, cachedLeft);
    return true;
}


bool ScopedDependentTransformer::setLeftRight(IHqlExpression * _left, IHqlExpression * _right, IHqlExpression * seq)
{
    ScopedTransformer::setLeftRight(_left, _right, seq);
    return false;
    pushChildContext(cachedRight, cachedRight);
    return true;
}


bool ScopedDependentTransformer::setTopLeftRight(IHqlExpression * ds, IHqlExpression * transformedDs, IHqlExpression * seq)
{
    ScopedTransformer::setTopLeftRight(ds, transformedDs, seq);
    pushChildContext(ds, transformedDs);
    return true;
}


void ScopedDependentTransformer::pushEvaluateScope(IHqlExpression * scope, IHqlExpression * transformedScope)
{
    ScopedTransformer::pushEvaluateScope(scope, transformedScope);
    pushChildContext(scope->queryNormalizedSelector(true), transformedScope->queryNormalizedSelector(true));
}

void ScopedDependentTransformer::popEvaluateScope()
{
    ScopedTransformer::popEvaluateScope();
    popChildContext();
}

void ScopedDependentTransformer::clearDataset(bool nested)                                      
{ 
    if (nested)
        popChildContext();
    ScopedTransformer::clearDataset(nested);
}

ANewTransformInfo * ScopedDependentTransformer::createTransformInfo(IHqlExpression * expr)
{
    return CREATE_NEWTRANSFORMINFO(MergingTransformInfo, expr);
}

void ScopedDependentTransformer::pushChildContext(IHqlExpression * expr, IHqlExpression * transformed)
{
    //NB: For this transformer it is the tables that are in scope that matter
    IHqlExpression * scope = expr->queryNormalizedSelector(false);
    //NB: Do no call createComma because that calls createDataset which unwinds a comma list!
    if (childScope)
        childScope.setown(createValue(no_comma,LINK(scope), childScope.getClear()));
    else
        childScope.set(scope);

    //Need map the selectors for all the datasets that are in scope - not just the current one.  If the same
    //one occurs twice, the more recent will take precedence.
    ForEachItemIn(idx, scopeStack)
    {
        ScopeInfo & cur = scopeStack.item(idx);
        IHqlExpression * dataset = cur.dataset;
        if (dataset)
            initializeActiveSelector(dataset, cur.transformedDataset);
    }
}

void ScopedDependentTransformer::popChildContext()
{
    if (childScope->getOperator() == no_comma)
        childScope.set(childScope->queryChild(1));
    else
        childScope.clear();
}


IHqlExpression * ScopedDependentTransformer::queryAlreadyTransformed(IHqlExpression * expr)
{
    AMergingTransformInfo * extra = queryExtra(expr);
    return extra->queryAlreadyTransformed(childScope);
}


void ScopedDependentTransformer::setTransformed(IHqlExpression * expr, IHqlExpression * transformed)
{
    AMergingTransformInfo * extra = queryExtra(expr);
    extra->setTransformed(childScope, transformed);
}


IHqlExpression * ScopedDependentTransformer::queryAlreadyTransformedSelector(IHqlExpression * expr)
{
    AMergingTransformInfo * extra = queryExtra(expr->queryNormalizedSelector());
    return extra->queryAlreadyTransformedSelector(childScope);
}


void ScopedDependentTransformer::setTransformedSelector(IHqlExpression * expr, IHqlExpression * transformed)
{
    assertex(expr == expr->queryNormalizedSelector());
    assertex(transformed == transformed->queryNormalizedSelector());
    AMergingTransformInfo * extra = queryExtra(expr->queryNormalizedSelector());
    extra->setTransformedSelector(childScope, transformed);
}


void ScopedDependentTransformer::suspendAllScopes(ScopeSuspendInfo & info)
{
    ScopedTransformer::suspendAllScopes(info);
    info.savedI.append(childScope.getClear());
}

void ScopedDependentTransformer::restoreScopes(ScopeSuspendInfo & info)
{
    childScope.setown(info.savedI.popGet());
    ScopedTransformer::restoreScopes(info);
}

//---------------------------------------------------------------------------

static HqlTransformerInfo splitterVerifierInfo("SplitterVerifier");
SplitterVerifier::SplitterVerifier() : NewHqlTransformer(splitterVerifierInfo)
{
}

void SplitterVerifier::analyseExpr(IHqlExpression * expr)
{
    if (expr->getOperator() == no_split)
    {
        SplitterVerifierInfo * extra = queryExtra(expr->queryBody());
        if (extra->useCount++)
        {
#ifdef _DEBUG
            IHqlExpression * id = expr->queryProperty(_uid_Atom);
            unsigned idValue = (id ? (unsigned)getIntValue(id->queryChild(0)) : 0);
#endif
            unsigned splitSize = (unsigned)getIntValue(expr->queryChild(1), 0);
            if (extra->useCount > splitSize)
                throwUnexpected();
            return;
        }
    }

    NewHqlTransformer::analyseExpr(expr);
}

void verifySplitConsistency(IHqlExpression * expr)
{
    SplitterVerifier checker;
    checker.analyse(expr, 0);
}

//---------------------------------------------------------------------------

static HqlTransformerInfo globalToParameterTransformerInfo("GlobalToParameterTransformer");
class GlobalToParameterTransformer : public QuickHqlTransformer
{
public:
    GlobalToParameterTransformer(HqlExprArray & _parameters, HqlExprArray & _defaults) 
        : QuickHqlTransformer(globalToParameterTransformerInfo, NULL), parameters(_parameters), defaults(_defaults)
    {}

    virtual IHqlExpression * createTransformedBody(IHqlExpression * expr);

protected:
    HqlExprArray & parameters;
    HqlExprArray & defaults;
};

IHqlExpression * GlobalToParameterTransformer::createTransformedBody(IHqlExpression * expr)
{
    if (expr->getOperator() != no_colon)
        return QuickHqlTransformer::createTransformedBody(expr);

    StringBuffer paramName;
    paramName.append("_implicit_hidden_").append(parameters.ordinality());
    HqlExprArray attrs;
    attrs.append(*createAttribute(_hidden_Atom));
    IHqlExpression * param = createParameter(createIdentifierAtom(paramName.str()), parameters.ordinality(), expr->getType(), attrs);
    parameters.append(*param);
    defaults.append(*LINK(expr));
    return LINK(param);
}

IHqlExpression * convertWorkflowToImplicitParmeters(HqlExprArray & parameters, HqlExprArray & defaults, IHqlExpression * expr)
{
    GlobalToParameterTransformer transformer(parameters, defaults);
    return transformer.transform(expr);
}


//---------------------------------------------------------------------------

static HqlTransformerInfo createRowSelectorExpanderInfo("CreateRowSelectorExpander");
class CreateRowSelectorExpander : public NewHqlTransformer
{
public:
    CreateRowSelectorExpander() : NewHqlTransformer(createRowSelectorExpanderInfo) {}

    virtual IHqlExpression * createTransformed(IHqlExpression * expr)
    {
        OwnedHqlExpr transformed = NewHqlTransformer::createTransformed(expr);

        if ((transformed->getOperator() == no_select) && transformed->hasProperty(newAtom))
        {
            IHqlExpression * child = transformed->queryChild(0);
            node_operator childOp = child->getOperator();
            if (childOp == no_createrow)
            {
                OwnedHqlExpr match = getExtractSelect(child->queryChild(0), transformed->queryChild(1));
                if (match)
                {
                    IHqlExpression * cur = queryUncastExpr(match);
                    switch (cur->getOperator())
                    {
                    case no_constant:
                    case no_select:
                        return match.getClear();
                    }
                }
            }
        }
        return transformed.getClear();
    }
};


IHqlExpression * expandCreateRowSelectors(IHqlExpression * expr)
{
    CreateRowSelectorExpander expander;
    return expander.createTransformed(expr);
}

//---------------------------------------------------------------------------

static HqlTransformerInfo hqlSelectorLocatorInfo("HqlSelectorLocator");
HqlSelectorLocator::HqlSelectorLocator(IHqlExpression * _selector) : NewHqlTransformer(hqlSelectorLocatorInfo)
{ 
    selector.set(_selector); 
    foundSelector = false; 
}


void HqlSelectorLocator::analyseExpr(IHqlExpression * expr)
{
    if (foundSelector || alreadyVisited(expr))
        return;
    unsigned numNonHidden = activityHidesSelectorGetNumNonHidden(expr, selector);
    if (numNonHidden != 0)
    {
        for (unsigned i=0; i < numNonHidden; i++)
            analyseExpr(expr->queryChild(i));
    }
    else
        NewHqlTransformer::analyseExpr(expr);
}

void HqlSelectorLocator::analyseSelector(IHqlExpression * expr)
{
    if (expr == selector)
    {
        foundSelector = true;
        return;
    }
    NewHqlTransformer::analyseSelector(expr);
}

bool HqlSelectorLocator::containsSelector(IHqlExpression * expr)
{
    foundSelector = false;
    analyse(expr, 0);
    return foundSelector;
}

HQL_API bool containsSelector(IHqlExpression * expr, IHqlExpression * selector)
{
    HqlSelectorLocator locator(selector);
    return locator.containsSelector(expr);
}

//---------------------------------------------------------------------------

static HqlTransformerInfo hqlSelectorAnywhereLocatorInfo("HqlSelectorLocator");
HqlSelectorAnywhereLocator::HqlSelectorAnywhereLocator(IHqlExpression * _selector) : NewHqlTransformer(hqlSelectorAnywhereLocatorInfo)
{ 
    selector.set(_selector); 
    foundSelector = false; 
}


void HqlSelectorAnywhereLocator::analyseExpr(IHqlExpression * expr)
{
    if (foundSelector || alreadyVisited(expr))
        return;
    NewHqlTransformer::analyseExpr(expr);
}

void HqlSelectorAnywhereLocator::analyseSelector(IHqlExpression * expr)
{
    if (expr == selector)
    {
        foundSelector = true;
        return;
    }
    NewHqlTransformer::analyseSelector(expr);
}

bool HqlSelectorAnywhereLocator::containsSelector(IHqlExpression * expr)
{
    foundSelector = false;
    analyse(expr, 0);
    return foundSelector;
}

HQL_API bool containsSelectorAnywhere(IHqlExpression * expr, IHqlExpression * selector)
{
    HqlSelectorAnywhereLocator locator(selector);
    return locator.containsSelector(expr);
}

//---------------------------------------------------------------------------

/*

  Notes:

  The following things are nasty when transforming graphs:
  conditional:
    should multiple conditional items be commoned up, or kept conditional.  There is no correct answer, but generally
    if there are any unconditional uses, then they should be commoned up, otherwise they should remain separate.
  
  sequential:
    nothing within the sequential should be moved before another item.  This messes up the conditional handling - since
    we can't move an unconditional evaluation before a conditional sequential one.

  Perfectly we would do the following:
  For each expression:
    For each sequential (and null sequential) section it is used from - maintained in order
       Is it used conditionally/unconditionally

  When transforming, would need to be current sequential branch dependent
    If already transformed unconditionally within a previous branch with the same or lower conditionallity then reuse
    If used unconditionally in this branch hoist within  branchand register
    else if conditional create a comma expression locally.

  Could possibly implement if no_sequential contained no_sequentialitem()
    no_sequential_item pushes a new mutex that inherits the previous values
    pops previous values when leaving
    - still causes problems for unconditional uses after conditional.

  In general too complicated to try and solve in a single pass....

  Try two passes.....
  1. Treat sequential as a new scope and don't common up between items.
  2. 
  */

#ifdef OPTIMIZE_TRANSFORM_ALLOCATOR
ANewHqlTransformer::ANewHqlTransformer()
{
    prevSize = beginTransformerAllocator();
}

ANewHqlTransformer::~ANewHqlTransformer()
{
    endTransformerAllocator(prevSize);
}

size32_t beginTransformerAllocator()
{
    transformerDepth++;
    return transformerHeap->allocated();
}

void endTransformerAllocator(size32_t prevSize)
{
    transformerDepth--;
    //More: Some way to specify minimum memory left unfreed??
    transformerHeap->setSize(prevSize);
}

void * transformerAlloc(size32_t size)
{
    return transformerHeap->alloc(size);
}
#endif

class ExpressionStatsInfo
{
public:
    enum { MaxOperands = 17 };
public:
    ExpressionStatsInfo() { count = 0; _clear(numOperands); countMax = 0; sumMax = 0; }

    void trace()
    {
        DBGLOG("numUnique %u", count);
        for (unsigned i=0; i < MaxOperands; i++)
            DBGLOG("  %u operands: %u", i, numOperands[i]);
        DBGLOG("  %u experessions total %u operands", countMax, sumMax);
    }

    unsigned count;
    unsigned numOperands[MaxOperands];
    unsigned countMax;
    unsigned sumMax;
};

static void calcNumUniqueExpressions(IHqlExpression * expr, ExpressionStatsInfo & info)
{
    if (expr->queryTransformExtra())
        return;
    expr->setTransformExtraUnlinked(expr);

    //use head recursion
    loop
    {
        info.count++;
        unsigned max = expr->numChildren();
        if (max >= ExpressionStatsInfo::MaxOperands)
        {
            info.countMax++;
            info.sumMax += max;
        }
        else
            info.numOperands[max]++;

        if (max == 0)
            return;

        for (unsigned idx=1; idx < max; idx++)
            calcNumUniqueExpressions(expr->queryChild(idx), info);
        expr = expr->queryChild(0);
    }
}

unsigned getNumUniqueExpressions(IHqlExpression * expr)
{
    TransformMutexBlock block;
    ExpressionStatsInfo info;
    calcNumUniqueExpressions(expr, info);
    return info.count;
}


unsigned getNumUniqueExpressions(const HqlExprArray & exprs)
{
    TransformMutexBlock block;
    ExpressionStatsInfo info;
    ForEachItemIn(i, exprs)
        calcNumUniqueExpressions(&exprs.item(i),info);
    return info.count;
}

//------------------------------------------------------------------------------------------------

static HqlTransformerInfo quickExpressionCounterInfo("QuickExpressionCounter");
class HQL_API QuickExpressionCounter : public QuickHqlTransformer
{
public:
    QuickExpressionCounter(IHqlExpression * _search, unsigned _limit) 
    : QuickHqlTransformer(quickExpressionCounterInfo, NULL), search(_search), limit(_limit)
    {
        matches = 0;
    }

    void analyse(IHqlExpression * expr)
    {
        if (expr == search)
            matches++;
        if (matches >= limit)
            return;
        QuickHqlTransformer::analyse(expr);
    }

    bool limitReached() const { return matches >= limit; }
    unsigned numMatches() const { return matches; }

protected:
    HqlExprAttr search;
    unsigned matches;
    unsigned limit;
};



extern HQL_API unsigned getNumOccurences(HqlExprArray & exprs, IHqlExpression * search, unsigned limit)
{
    QuickExpressionCounter counter(search, limit);
    ForEachItemIn(i, exprs)
        counter.analyse(&exprs.item(i));
    return counter.numMatches();
}


extern HQL_API void logTreeStats(IHqlExpression * expr)
{
    TransformMutexBlock block;
    ExpressionStatsInfo info;
    calcNumUniqueExpressions(expr,info);
    info.trace();
}

extern HQL_API void logTreeStats(const HqlExprArray & exprs)
{
    TransformMutexBlock block;
    ExpressionStatsInfo info;
    ForEachItemIn(i, exprs)
        calcNumUniqueExpressions(&exprs.item(i),info);
    info.trace();
}

//------------------------------------------------------------------------------------------------

static HqlTransformerInfo selectCollectingTransformerInfo("SelectCollectingTransformer");
class SelectCollectingTransformer : public NewHqlTransformer
{
public:
    SelectCollectingTransformer(HqlExprArray & _found)
    : NewHqlTransformer(selectCollectingTransformerInfo), found(_found)
    {
    }

    virtual void analyseExpr(IHqlExpression * expr)
    {
        if (alreadyVisited(expr))
            return;
        if (expr->getOperator() == no_select)
        {
            if (!found.contains(*expr))
                found.append(*LINK(expr));
            return;
        }
        NewHqlTransformer::analyseExpr(expr);
    }

protected:
    HqlExprArray & found;
};


void gatherSelectExprs(HqlExprArray & target, IHqlExpression * expr)
{
    SelectCollectingTransformer collector(target);
    collector.analyse(expr, 0);
}

//------------------------------------------------------------------------------------------------

static HqlTransformerInfo fieldAccessAnalyserInfo("FieldAccessAnalyser");
FieldAccessAnalyser::FieldAccessAnalyser(IHqlExpression * _selector) : NewHqlTransformer(fieldAccessAnalyserInfo), selector(_selector)
{
    unwindFields(fields, selector->queryRecord());
    numAccessed = 0;
    accessed.setown(createBitSet());
}

IHqlExpression * FieldAccessAnalyser::queryLastFieldAccessed() const
{
    if (numAccessed == 0)
        return NULL;
    if (accessedAll())
        return &fields.tos();
    ForEachItemInRev(i, fields)
    {
        if (accessed->test(i))
            return &fields.item(i);
    }
    throwUnexpected();
}

void FieldAccessAnalyser::analyseExpr(IHqlExpression * expr)
{
    if (accessedAll() || alreadyVisited(expr))
        return;
    if (expr == selector)
    {
        setAccessedAll();
        return;
    }
    if (expr->getOperator() == no_select)
    {
        if (expr->queryChild(0) == selector)
        {
            unsigned match = fields.find(*expr->queryChild(1));
            assertex(match != NotFound);
            if (!accessed->test(match))
            {
                accessed->set(match);
                numAccessed++;
            }
            return;
        }
    }
    NewHqlTransformer::analyseExpr(expr);
}


void FieldAccessAnalyser::analyseSelector(IHqlExpression * expr)
{
    if (expr == selector)
    {
        setAccessedAll();
        return;
    }
    if (expr->getOperator() == no_select)
    {
        if (expr->queryChild(0) == selector)
        {
            unsigned match = fields.find(*expr->queryChild(1));
            assertex(match != NotFound);
            if (!accessed->test(match))
            {
                accessed->set(match);
                numAccessed++;
            }
            return;
        }
    }
    NewHqlTransformer::analyseSelector(expr);
}


//------------------------------------------------------------------------------------------------

/*

Some notes on selector ambiguity.

a) Whatever work is done, it is going to be impossible to completely get rid of ambiguious selectors.

It may be possible with no_left/no_right, depending on the implementation, but not with dataset selectors. 
E.g, x(f1 in set(x(f2), f3))
inside the evaluation of f3, the code generator needs to correctly ensure that
i) the inner iterator on x(f2) is used when evaluating f3.
ii) f3 is never hoisted from the inner loop to the outer.

=>
i) ALL selector replacement needs to take dataset nesting into account (even non no_left/no_right)
ii) Need to never evaluate an expression (e.g., alias) in the parent if it will change the meaning.
    => Need to tag inherited rows, and don't evaluate in parent if they clash with required rows.
    => Need to be very careful in any hoisting code (resource/cse etc.)

b) LEFT and RIGHTT really should be unique to the point they are used.
=> We need to associate some kind of uid with each project.  Considered the following options:
i) create a uid each time an expression that defines LEFT/RIGHT is created.  But mangle it with the parameters to ensure really unique.
ii) create a uid based on some function of the input datasets.
iii) create a uid based on some function of the normalized selectors for the input datasets.

(i) Creates a large number of complex expressions, and removes quite a lot of accidental commoning up which occurs.
(ii) should be ok, but still signficantly increases the number of unique expressions
(iii) and improvement on (ii), but doesn't fare much better.



*/