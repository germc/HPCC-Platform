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
#include "platform.h"
#include <algorithm>

#include "jliball.hpp"

#include "jlib.hpp"
#include "jexcept.hpp"
#include "jmisc.hpp"
#include "javahash.hpp"
#include "jmd5.hpp"
#include "jfile.hpp"
#include "eclhelper.hpp"

#include "hql.hpp"
#include "hqlfunc.hpp"
#include "hqlattr.hpp"
#include "hqlcpp.ipp"
#include "hqlwcpp.hpp"
#include "hqlcpputil.hpp"
#include "hqlres.hpp"
#include "hqlerror.hpp"
#include "hqlcerrors.hpp"
#include "hqlcatom.hpp"
#include "hqlpmap.hpp"
#include "hqlthql.hpp"
#include "hqlfold.hpp"
#include "eclrtl.hpp"
#include "hqllib.ipp"
#include "hqlnlp.ipp"
#include "hqlutil.hpp"
#include "hqltcppc.ipp"
#include "hqlttcpp.ipp"
#include "hqlccommon.hpp"
#include "hqlopt.hpp"
#include "hqlpopt.hpp"
#include "hqlcse.ipp"
#include "thorplugin.hpp"

static CBuildVersion _bv("$HeadURL: https://svn.br.seisint.com/ecl/trunk/ecl/hqlcpp/hqlcpp.cpp $ $Id: hqlcpp.cpp 66009 2011-07-06 12:28:32Z ghalliday $");

#ifdef _DEBUG
//#define ADD_ASSIGNMENT_COMMENTS
//#define ADD_RESOURCE_AS_CPP_COMMENT
#endif

//Defaults for various options.
#define COMPLEXITY_TO_HOIST             2
#define INLINE_COMPARE_THRESHOLD        2 //7           // above this, a loop is generated
#define MAX_NESTED_CASES                8
#define MAX_SIMPLE_VAR_SIZE             99999
#define MAX_STATIC_ROW_SIZE             10000
#define MAX_LOCAL_ROW_SIZE              32
#define DEFAULT_NLP_DETAIL              1


#ifdef _WIN32
#define DEFAULT_ACTIVITIES_PER_CPP      800             // windows compiler is fast, linker is slow, but compiler also has quite a small compile limit
#else
#define DEFAULT_ACTIVITIES_PER_CPP      500             // gcc assembler is v.slow
#endif

//MORE: Simple vars don't work if they are made class members...

//#define SEARCH_VARIABLE  "v211"

//===========================================================================

static CriticalSection * systemCS;
static IHqlScope * cppSystemScope;

//===========================================================================

MODULE_INIT(INIT_PRIORITY_STANDARD)
{
    systemCS = new CriticalSection;
    return true;
}
MODULE_EXIT()
{
    ::Release(cppSystemScope);
    cppSystemScope = NULL;
    delete systemCS;
}


//---------------------------------------------------------------------------

class SubStringInfo : public SubStringHelper
{
public:
    SubStringInfo(IHqlExpression * _expr) : SubStringHelper(_expr) { expr = _expr; }

    void bindToFrom(HqlCppTranslator & translator, BuildCtx & ctx);

public:
    IHqlExpression * expr;
    CHqlBoundExpr boundFrom;
    CHqlBoundExpr boundTo;
};

void SubStringInfo::bindToFrom(HqlCppTranslator & translator, BuildCtx & ctx)
{
    if (to && to->isAttribute())
        throwError(HQLERR_StarRangeOnlyInJoinCondition);

    if (from == to)
    {
        if (from)
        {
            translator.buildSimpleExpr(ctx, from, boundFrom);
            boundTo.expr.set(boundFrom.expr);
        }
    }
    else
    {
        if (from)
            translator.buildCachedExpr(ctx, from, boundFrom);
        if (to)
            translator.buildCachedExpr(ctx, to, boundTo);
    }
}

//---------------------------------------------------------------------------

IHqlExpression * DatasetReference::querySelector() const
{
    if (side == no_none)
        return ds->queryNormalizedSelector();
    return selector;
}

IHqlExpression * DatasetReference::querySelSeq() const
{
    if (side == no_none)
        return NULL;
    return selector->queryChild(1);
}

IHqlExpression * DatasetReference::mapCompound(IHqlExpression * expr, IHqlExpression * to) const
{
    return replaceSelector(expr, querySelector(), to);
}

IHqlExpression * DatasetReference::mapScalar(IHqlExpression * expr, IHqlExpression * to) const
{
    return replaceSelector(expr, querySelector(), to);
}

//---------------------------------------------------------------------------

extern ClusterType getClusterType(const char * platform, ClusterType dft)
{
    if (stricmp(platform, "thor") == 0)
        return ThorCluster;
    if (stricmp(platform, "thorlcr") == 0)
        return ThorLCRCluster;
    if (stricmp(platform, "hthor") == 0)
        return HThorCluster;
    if (stricmp(platform, "roxie") == 0)
        return RoxieCluster;
    return dft;
}

IHqlExpression * createVariable(ITypeInfo * type)
{
    StringBuffer tempName;
    getUniqueId(tempName.append('v'));

#ifdef _DEBUG
#ifdef SEARCH_VARIABLE
    if (stricmp(tempName.str(), SEARCH_VARIABLE)==0)
        type = type;
#endif
#endif

    return ::createVariable(tempName.str(), type);
}

IHqlExpression * convertWrapperToPointer(IHqlExpression * expr)
{
    ITypeInfo * type = expr->queryType();
    if (hasWrapperModifier(type))
        return createValue(no_implicitcast, makeReferenceModifier(removeModifier(type, typemod_wrapper)), LINK(expr));
    return LINK(expr);
}

IHqlExpression * ensureIndexable(IHqlExpression * expr)
{
    ITypeInfo * type = expr->queryType();
    if (type->getTypeCode() == type_data)
    {
        IHqlExpression * base = queryStripCasts(expr);
        return createValue(no_implicitcast, makeReferenceModifier(makeStringType(type->getSize(), NULL, NULL)), LINK(base));
    }

    return convertWrapperToPointer(expr);
}

void extendConjunctionOwn(HqlExprAttr & cond, IHqlExpression * next)
{
    if (cond)
        next = createBoolExpr(no_and, cond.getClear(), next);
    cond.setown(next);
}

inline bool isPushed(const IHqlExpression * expr)
{
    return (expr->getOperator() == no_decimalstack);
}

inline bool isPushed(const CHqlBoundExpr & bound)
{
    return isPushed(bound.expr);
}

bool isSimpleTranslatedStringExpr(IHqlExpression * expr)
{
    loop
    {
        node_operator op = expr->getOperator();

        switch (op)
        {
        case no_constant:
        case no_variable:
        case no_callback:
            return true;
        case no_cast:
        case no_implicitcast:
        case no_typetransfer:
        case no_deref:
        case no_address:
            expr = expr->queryChild(0);
            break;
        case no_add:
        case no_sub:
            if (!isSimpleTranslatedStringExpr(expr->queryChild(1)))
                return false;
            expr = expr->queryChild(0);
            break;
        default:
            return false;
        }
    }
}

bool isSimpleTranslatedExpr(IHqlExpression * expr)
{
    switch (expr->queryType()->getTypeCode())
    {
    case type_data:
    case type_string:
    case type_qstring:
    case type_varstring:
    case type_decimal:
        //Less strict rules for strings (and decimal), because string temporaries are more expensive.
        return isSimpleTranslatedStringExpr(expr);
    case type_set:
        //for the moment assume set expressions are always simple once translated.
        return true;
    }

    loop
    {
        node_operator op = expr->getOperator();

        switch (op)
        {
        case no_constant:
        case no_variable:
        case no_callback:
        case no_nullptr:
            return true;
        case no_typetransfer:
            expr = expr->queryChild(0);
            break;
        default:
            return false;
        }
    }
}

bool isFixedLengthList(IHqlExpression * expr)
{
    switch (expr->getOperator())
    {
    case no_list:
    case no_datasetlist:
    case no_sortlist:
        return true;
    }
    return false;
}

bool needVarStringCompare(ITypeInfo * leftType, ITypeInfo * rightType)
{
    unsigned lSize = leftType->getSize();
    unsigned rSize = rightType->getSize();
    return (lSize != rSize) || (lSize == UNKNOWN_LENGTH);
}


_ATOM queryStrCompareFunc(ITypeInfo * realType)
{
    switch (realType->getTypeCode())
    {
    case type_data:
        return compareDataDataAtom;
    case type_qstring:
        return compareQStrQStrAtom;
    }
    ICharsetInfo * charset = realType->queryCharset();
    _ATOM charsetName = charset->queryName();
    if (charsetName == dataAtom)
        return compareDataDataAtom;
    if (charsetName == asciiAtom)
        return compareStrStrAtom;
    if (charsetName == ebcdicAtom)
        return compareEStrEStrAtom;
    assertex(!"Unknown string comparison");
    return compareStrStrAtom;
}

IHqlExpression * getAddress(IHqlExpression * expr)
{
    if (expr->getOperator() == no_deref)
    {
        IHqlExpression * address = expr->queryChild(0);
        return LINK(address);
    }

    return createValue(no_address, makePointerType(expr->getType()), LINK(expr));
}


IHqlExpression * getRawAddress(IHqlExpression * expr)
{
    OwnedHqlExpr raw = getAddress(expr);
    loop
    {
        switch (raw->getOperator())
        {
        case no_cast:
        case no_implicitcast:
            break;
        default:
            return raw.getClear();
        }
        raw.set(raw->queryChild(0));
    }
}

IHqlExpression * getPointer(IHqlExpression * source)
{
    if (source->getOperator() == no_constant)
        return LINK(source);

    ITypeInfo * type = source->queryType();
    Owned<ITypeInfo> newType;
    switch (type->getTypeCode())
    {
    case type_set:
        if (type->isReference())
            return LINK(source);
        newType.setown(makeReferenceModifier(LINK(queryUnqualifiedType(type))));
        if (hasWrapperModifier(type))
        {
            OwnedHqlExpr cast = createValue(no_implicitcast, LINK(newType), LINK(source));
            return createValue(no_typetransfer, LINK(newType), LINK(cast));
        }
        break;
    case type_table:
    case type_groupedtable:
    case type_row:
    case type_decimal:
    case type_string:
    case type_data:
    case type_qstring:
    case type_varstring:
    case type_unicode:
    case type_varunicode:
    case type_utf8:
        if (isTypePassedByAddress(type))
        {
            if (type->isReference())
                return LINK(source);
            newType.setown(makeReferenceModifier(LINK(queryUnqualifiedType(type))));
            if (hasLinkCountedModifier(type))
                newType.setown(makeAttributeModifier(newType.getClear(), getLinkCountedAttr()));
            if (hasWrapperModifier(type))
                return createValue(no_implicitcast, LINK(newType), LINK(source));
        //An array of X is implicitly converted to pointer to X so no need to do &a[0]
            return createValue(no_typetransfer, LINK(newType), LINK(source));
        }
        else
        {
            newType.setown(removeModifier(type, typemod_wrapper));
            if (hasWrapperModifier(type))
                return createValue(no_implicitcast, LINK(newType), LINK(source));
            return LINK(source);
        }
        break;
    case type_pointer:
        return LINK(source);
    default:
        newType.setown(makePointerType(LINK(type)));
        break;
    }

    IHqlExpression * cur = source;
    while (cur->getOperator() == no_typetransfer)
        cur = cur->queryChild(0);
    if (cur->getOperator() == no_deref)
    {
        IHqlExpression * address = cur->queryChild(0);
        if (address->queryType() == newType)
            return LINK(address);

        return createValue(no_implicitcast, newType.getClear(), LINK(address));
    }
    else
        return createValue(no_address, newType.getClear(), LINK(source));

}


bool isChildOf(IHqlExpression * parent, IHqlExpression * child)
{
    unsigned max = parent->numChildren();
    unsigned idx;
    for (idx = 0; idx < max; idx++)
    {
        IHqlExpression * cur = parent->queryChild(idx);
        if (cur == child)
            return true;
    }
    return false;
}

bool canRemoveStringCast(ITypeInfo * to, ITypeInfo * from)
{
    unsigned fromSize = from->getSize();
    unsigned toSize = to->getSize();

    //Special case string conversions that don't require us to copy any data.
    if ((toSize == UNKNOWN_LENGTH) || ((fromSize != UNKNOWN_LENGTH) && (toSize <= fromSize)))
    {
        switch (from->getTypeCode())
        {
        case type_varstring:
            if (toSize != UNKNOWN_LENGTH)
                break;
            //fall through
        case type_data:
        case type_string:
            {
                ICharsetInfo * srcset = from->queryCharset();
                ICharsetInfo * tgtset = to->queryCharset();
                
                //Data never calls a conversion function...
                if ((srcset == tgtset) || (to->getTypeCode() == type_data) || (from->getTypeCode() == type_data))
                    return true;
            }
        case type_qstring:
            return false;
        }
    }
    return false;
}


bool isProjectedInRecord(IHqlExpression * record, IHqlExpression * expr)
{
    unsigned max = record->numChildren();
    unsigned idx;
    for (idx = 0; idx < max; idx++)
    {
        IHqlExpression * cur = record->queryChild(idx);
        if (cur->queryChild(0) == expr)
            return true;
    }
    return false;
}

IHqlExpression * queryStripCasts(IHqlExpression * expr)
{
    while ((expr->getOperator() == no_cast) || (expr->getOperator() == no_implicitcast))
        expr = expr->queryChild(0);
    return expr;
}


// Format the list is stored in doesn't matter, so allow constant strings to be stored by reference
IHqlExpression * getOptimialListFormat(IHqlExpression * table)
{
    if (table->isConstant() && table->getOperator() == no_list)
    {
        ITypeInfo * elemType = table->queryType()->queryChildType();
        if (!elemType->isReference())
        {
            switch (elemType->getTypeCode())
            {
            case type_string:
            case type_data:
                {
                    HqlExprArray args;
                    table->unwindList(args, no_list);
                    return createValue(no_list, makeSetType(makeReferenceModifier(LINK(elemType))), args);
                }
            }
        }
    }
    return LINK(table);
}


bool canOptimizeAdjust(IHqlExpression * value)
{
    ITypeInfo * type = value->queryType();
    switch (value->getOperator())
    {
    case no_constant:
        return true;
    case no_add:
    case no_sub:
        return value->queryChild(1)->queryValue() != NULL;
    }
    return false;
}

IHqlExpression * adjustValue(IHqlExpression * value, __int64 delta)
{
    if (delta == 0)
        return LINK(value);

    ITypeInfo * type = value->queryType();
    switch (value->getOperator())
    {
    case no_constant:
        {
            __int64 newValue = value->queryValue()->getIntValue()+delta;
            if (type == sizetType)
                return getSizetConstant((size32_t)newValue);
            return createConstant(type->castFrom(true, newValue));
        }
    case no_add:
        {
            IHqlExpression * lhs = value->queryChild(0);
            IHqlExpression * rhs = value->queryChild(1);
            IValue * rhsValue = rhs->queryValue();
            IValue * lhsValue = lhs->queryValue();
            if (rhsValue)
            {
                delta += rhsValue->getIntValue();
                if (delta == 0)
                    return LINK(lhs);
                value = lhs;
            }
            else if (lhsValue)
            {
                delta += lhsValue->getIntValue();
                if (delta == 0)
                    return LINK(rhs);
                value = rhs;
            }
            else if (canOptimizeAdjust(rhs))
                return createValue(no_add, value->getType(), LINK(lhs), adjustValue(rhs, delta));
            break;
        }
    case no_sub:
        {
            IValue * rhsValue = value->queryChild(1)->queryValue();
            if (rhsValue)
            {
                IHqlExpression * lhs = value->queryChild(0);
                delta -= rhsValue->getIntValue();
                if (delta == 0)
                    return LINK(lhs);
                value = lhs;
            }
            break;
        }
    case no_translated:
        {
            IHqlExpression * arg = value->queryChild(0);
            if (arg->queryValue())
            {
                OwnedHqlExpr newValue = adjustValue(arg, delta);
                return createTranslated(newValue);
            }
            break;
        }
    }

    IHqlExpression * deltaExpr;
    node_operator op = no_add;
    if (delta < 0)
    {
        op = no_sub;
        delta = -delta;
    }

    if (type == sizetType || !type->isInteger())
        deltaExpr = getSizetConstant((size32_t)delta);
    else
        deltaExpr = createConstant(type->castFrom(true, delta));

    return createValue(op, LINK(type), LINK(value), deltaExpr);
}

IHqlExpression * adjustIndexBaseToZero(IHqlExpression * index)
{
    return adjustValue(index, -1);
}


IHqlExpression * adjustIndexBaseToOne(IHqlExpression * index)
{
    return adjustValue(index, +1);
}


IHqlExpression * adjustBoundIntegerValues(IHqlExpression * left, IHqlExpression * right, bool subtract)
{
    assertex(queryUnqualifiedType(left->queryType()) == queryUnqualifiedType(right->queryType()));
    if (canOptimizeAdjust(left))
    {
        node_operator op = right->getOperator();
        switch (op)
        {
        case no_constant:
            {
                __int64 rhsValue = right->queryValue()->getIntValue();
                if (subtract)
                    rhsValue = -rhsValue;
                return adjustValue(left, rhsValue);
            }
        case no_add:
        case no_sub:
            {
                IHqlExpression * rl = right->queryChild(0);
                IHqlExpression * rr = right->queryChild(1);
                if (rr->getOperator() == no_constant)
                {
                    ITypeInfo * rlt = rl->queryType();
                    ITypeInfo * rrt = rr->queryType();
                    if (queryUnqualifiedType(rl->queryType()) == queryUnqualifiedType(rr->queryType()))
                    {
                        __int64 delta = rr->queryValue()->getIntValue();
                        if (op == no_sub)
                            delta = -delta;
                        if (subtract)
                            delta = -delta;
                        OwnedHqlExpr newLeft = adjustValue(left, delta);
                        return adjustBoundIntegerValues(newLeft, rl, subtract);
                    }
                }
                break;
            }
        }
    }

    switch (left->getOperator())
    {
    case no_constant:
        if (!subtract)
            return adjustBoundIntegerValues(right, left, false);
        break;
    case no_add:
        {
            IHqlExpression * lr = left->queryChild(1);
            if (lr->getOperator() == no_constant)
            {
                OwnedHqlExpr newLeft = adjustBoundIntegerValues(left->queryChild(0), right, subtract);
                return adjustBoundIntegerValues(newLeft, lr, false);
            }
            break;
        }
    case no_variable:
        if (!subtract && (right->getOperator() == no_add) && (right->queryChild(1)->getOperator() == no_constant))
        {
            OwnedHqlExpr temp = adjustBoundIntegerValues(left, right->queryChild(0), false);
            return adjustBoundIntegerValues(temp, right->queryChild(1), false);
        }
        break;
    }

    return createValue(subtract ? no_sub : no_add, left->getType(), LINK(left), LINK(right));
}

    
IHqlExpression * multiplyValue(IHqlExpression * expr, unsigned __int64 value)
{
    if (isZero(expr))
        return LINK(expr);
    ITypeInfo * type = expr->queryType();
    IValue * exprValue = expr->queryValue();
    if (exprValue && type->isInteger())
        return createConstant(type->castFrom(false, exprValue->getIntValue() * value));
    if (expr->getOperator() == no_translated)
    {
        IHqlExpression * translated = expr->queryChild(0);
        if (translated->queryValue())
        {
            OwnedHqlExpr newValue = multiplyValue(translated, value);
            return createTranslated(newValue);
        }
    }

    return createValue(no_mul, LINK(type), LINK(expr), createConstant(type->castFrom(false, value)));
}

bool matchesConstValue(IHqlExpression * expr, __int64 matchValue)
{
    IValue * value = expr->queryValue();
    if (value)
        return value->getIntValue() == matchValue;
    if (expr->getOperator() == no_translated)
        return matchesConstValue(expr->queryChild(0), matchValue);
    return false;
}


IHqlExpression * createTranslated(IHqlExpression * expr)
{
    return createValue(no_translated, expr->getType(), LINK(expr));
}

IHqlExpression * createTranslatedOwned(IHqlExpression * expr)
{
    return createValue(no_translated, expr->getType(), expr);
}

IHqlExpression * createTranslated(IHqlExpression * expr, IHqlExpression * length)
{
    ITypeInfo * type = expr->queryType();
    switch (type->getTypeCode())
    {
    case type_table:
    case type_groupedtable:
        return createDataset(no_translated, LINK(expr), LINK(length));
    }
    return createValue(no_translated, expr->getType(), LINK(expr), LINK(length));
}

static IHqlExpression * querySimplifyCompareArgCast(IHqlExpression * expr)
{
    if (expr->isConstant())
        return expr;
    while ((expr->getOperator() == no_implicitcast) || (expr->getOperator() == no_cast))
    {
        ITypeInfo * type = expr->queryType()->queryPromotedType();
        switch (type->getTypeCode())
        {
        case type_string:
        case type_data:
        case type_unicode:
        case type_qstring:
        case type_utf8:
            break;
        default:
            return expr;
        }
        IHqlExpression * child = expr->queryChild(0);
        ITypeInfo * childType = child->queryType()->queryPromotedType();
        if (type->getStringLen() < childType->getStringLen())
            break;
        type_t tc = type->getTypeCode();
        if (tc != childType->getTypeCode())
        {
            if (tc == type_string)
            {
                if (childType->getTypeCode() != type_varstring)
                    break;
                if (type->queryCharset() != childType->queryCharset())
                    break;
            }
            else if (tc == type_unicode)
            {
                if (childType->getTypeCode() != type_varunicode)
                    break;
                if (type->queryLocale() != childType->queryLocale())
                    break;
            }
            else
                break;
        }
        else
        {
            Owned<ITypeInfo> stretched = getStretchedType(type->getStringLen(), childType);
            if (stretched != type)
                break;
        }
        expr = child;
    }
    return expr;
}


IHqlExpression * getSimplifyCompareArg(IHqlExpression * expr)
{
    IHqlExpression * cast = querySimplifyCompareArgCast(expr);
    if (cast->getOperator() != no_substring)
        return LINK(cast);
    if (cast->queryChild(0)->queryType()->getTypeCode() == type_qstring)
        return LINK(cast);
    HqlExprArray args;
    unwindChildren(args, cast);
    args.append(*createAttribute(quickAtom));
    return cast->clone(args);
}


bool isNullAssign(const CHqlBoundTarget & target, IHqlExpression * expr)
{
    ITypeInfo * targetType = target.expr->queryType();
    //if an assignment to a local variable size temporary object, then it is ok to omit an assignment of null
    //since it won't change its value, and it isn't going to be assigned more than once.
    if ((targetType->getSize() == UNKNOWN_LENGTH) && target.length && hasWrapperModifier(targetType) && !hasModifier(targetType, typemod_member))
    {
        ITypeInfo * exprType = expr->queryType();
        switch (exprType->getTypeCode())
        {
        case type_data:
        case type_string:
        case type_qstring:
            return exprType->getSize() == 0;
        case type_table:
            return expr->getOperator() == no_null;
        }
    }
    return false;
}

ExpressionFormat queryNaturalFormat(ITypeInfo * type)
{
    if (hasOutOfLineModifier(type))
        return FormatArrayDataset;
    if (hasLinkCountedModifier(type))
        return FormatLinkedDataset;
    return FormatBlockedDataset;
}


//===========================================================================

SubGraphInfo::SubGraphInfo(IPropertyTree * _tree, unsigned _id, IHqlExpression * _graphTag, SubGraphType _type) 
    : HqlExprAssociation(subGraphMarker), tree(_tree) 
{ 
    id = _id; 
    type = _type; 
    graphTag.set(_graphTag);
}

//===========================================================================

IHqlExpression * CHqlBoundExpr::getTranslatedExpr() const
{
    HqlExprArray args;
    args.append(*LINK(expr));
    if (length) args.append(*LINK(length));
    if (count)  args.append(*createAttribute(countAtom, LINK(count)));
    if (isAll)  args.append(*createAttribute(allAtom, LINK(isAll)));

    ITypeInfo * type = expr->queryType();
    switch (type->getTypeCode())
    {
    case type_table:
    case type_groupedtable:
        return createDataset(no_translated, args);
    }
    return createValue(no_translated, LINK(type), args);
}

IHqlExpression * CHqlBoundExpr::getIsAll() const
{
    if (isAll)
        return LINK(isAll);
    return LINK(queryBoolExpr(false));
}


void CHqlBoundExpr::setFromTarget(const CHqlBoundTarget & target)
{
    isAll.set(target.isAll);
    count.set(target.count);
    length.set(target.length);
    expr.setown(convertWrapperToPointer(target.expr));
}

void CHqlBoundExpr::setFromTranslated(IHqlExpression * translatedExpr)
{
    expr.set(translatedExpr->queryChild(0));
    IHqlExpression * arg = translatedExpr->queryChild(1);
    if (arg)
    {
        unsigned i = 2;
        if (arg->getOperator() != no_attr)
        {
            length.set(arg);
            arg = translatedExpr->queryChild(i++);
        }
        while (arg)
        {
            _ATOM name = arg->queryName();
            if (name == countAtom)
                count.set(arg->queryChild(0));
            else if (name == allAtom)
                isAll.set(arg->queryChild(0));
            else
                UNIMPLEMENTED;
            arg = translatedExpr->queryChild(i++);
        }
    }
}

bool CHqlBoundTarget::extractFrom(const CHqlBoundExpr & bound)
{
    ITypeInfo * boundType = bound.queryType();
    if (bound.count)
    {
        if (bound.count->getOperator() != no_variable)
            return false;
        if (!hasLinkCountedModifier(boundType))
            return false;
    }

    if (bound.isAll)
    {
        assertex(bound.isAll->getOperator() == no_variable);
    }
    else if (boundType->getTypeCode() == type_set)
        return false;

    if (bound.length)
    {
        if (bound.length->getOperator() != no_variable)
            return false;
    }
    else if (boundType->getSize() == UNKNOWN_LENGTH)
    {
        type_t btc = boundType->getTypeCode();
        if ((btc != type_varstring) && (btc != type_varunicode) && !hasLinkCountedModifier(boundType))
            return false;
    }

    IHqlExpression * boundExpr = bound.expr;
    if (boundExpr->getOperator() == no_implicitcast)
    {
        IHqlExpression * uncast = boundExpr->queryChild(0);
        if (hasModifier(uncast->queryType(), typemod_member) && 
            (queryUnqualifiedType(boundExpr->queryType()) == queryUnqualifiedType(uncast->queryType())))
            boundExpr = uncast;
    }
    expr.set(boundExpr);
    isAll.set(bound.isAll);
    length.set(bound.length);
    count.set(bound.count);
    return true;
}


bool CHqlBoundTarget::isFixedSize() const
{ 
    validate();
    return queryType()->getSize() != UNKNOWN_LENGTH;
}


void CHqlBoundTarget::validate() const
{ 
    if (expr)
    {
        if (queryType()->getSize() != UNKNOWN_LENGTH)
        {
            assertex(!length);
        }
        else if (isArrayRowset(queryType()))
            assertex(count);
        else
        {
            assertex(length || queryType()->getTypeCode() == type_varstring || queryType()->getTypeCode() == type_varunicode);
        }
    }
}


ITypeInfo * CHqlBoundTarget::queryType() const
{
    return expr->queryType();
}

IHqlExpression * CHqlBoundTarget::getTranslatedExpr() const
{
    CHqlBoundExpr temp;
    temp.setFromTarget(*this);
    return temp.getTranslatedExpr();
}


//===========================================================================

CompoundBuilder::CompoundBuilder(node_operator _op)
{
    op = _op;
}


void CompoundBuilder::addOperand(IHqlExpression * arg)
{
    if (first.get())
    {
        compound.setown(createOpenValue(op, makeBoolType()));
        compound->addOperand(first.getClear());
    }
    if (compound)
        compound->addOperand(arg);
    else
        first.setown(arg);
}

IHqlExpression * CompoundBuilder::getCompound()
{
    if (compound)
        return compound.getClear()->closeExpr();
    return first.getClear();
}

//===========================================================================

void buildClearPointer(BuildCtx & ctx, IHqlExpression * expr, CompilerType compiler)
{
    StringBuffer s;
    generateExprCpp(s, expr, compiler).append("=NULL;");
    ctx.addQuoted(s);
}

void insertUniqueString(StringAttrArray & array, const char * text)
{
    ForEachItemIn(idx, array)
    {
        StringAttrItem & cur = array.item(idx);
        if (stricmp(cur.text, text) == 0)
            return;
    }
    array.append(* new StringAttrItem(text));
}

HqlCppInstance::HqlCppInstance(IWorkUnit *_wu, const char * _wupathname)
{
    workunit.set(_wu);
    wupathname.set(_wupathname);
}

HqlStmts * HqlCppInstance::ensureSection(_ATOM section)
{
    HqlStmts * match = querySection(section);
    if (match)
        return match;

    HqlCppSection * cur = new HqlCppSection;
    cur->section = section;
    sections.append(*cur);
    return &cur->stmts;
}

void HqlCppInstance::processIncludes()
{
    BuildCtx ctx(*this, includeAtom);
    StringBuffer s;

    ForEachItemIn(idx, includes)
    {
        s.clear().append("#include \"").append(includes.item(idx).text).append("\"");
        ctx.addQuoted(s);
    }
}

const char * HqlCppInstance::queryLibrary(unsigned idx)
{
    if (modules.isItem(idx))
        return modules.item(idx).text;
    return NULL;
}

HqlStmts * HqlCppInstance::querySection(_ATOM section)
{
    ForEachItemIn(idx, sections)
    {
        HqlCppSection & cur = (HqlCppSection &)sections.item(idx);
        if (cur.section == section)
            return &cur.stmts;
    }
    return NULL;
}

void HqlCppInstance::addPlugin(const char *plugin, const char *version, bool inThor)
{
    if (!plugin || !*plugin)
        return;

    StringBuffer dllname(plugin);
    getFileNameOnly(dllname, false); // MORE - shouldn't really need to do this here....

    if (workunit)
    {
        Owned<IWUPlugin> p = workunit->updatePluginByName(dllname.str());

        p->setPluginVersion(version);
        if (inThor)
            p->setPluginThor(true);
        else
            p->setPluginHole(true);
    }
    if (!plugins)
        plugins.setown(createPTree("Plugins", false));
    StringBuffer xpath;
    xpath.append("Plugin[@dll='").append(dllname).append("']");
    if (!plugins->hasProp(xpath.str()))
    {
        IPropertyTree * pluginNode = createPTree("Plugin", false);
        pluginNode->setProp("@dll", dllname.str());
        pluginNode->setProp("@version", version);
        plugins->addPropTree("Plugin", pluginNode);
    }
}


void HqlCppInstance::addPluginsAsResource()
{
    if (!plugins)
        return;
    StringBuffer pluginXML;
    toXML(plugins, pluginXML);
    addResource("PLUGINS", 1, pluginXML.length(), pluginXML.str());
}


bool HqlCppInstance::useFunction(IHqlExpression * func)
{
    assertex(func);
    func = func->queryBody();
    if (helpers.contains(*func))
        return false;

    helpers.append(*LINK(func));

    IHqlExpression * funcDef = func->queryChild(0);
    StringBuffer libname, init, include;
    getProperty(funcDef, libraryAtom, libname);
    getProperty(funcDef, initfunctionAtom, init);
    getProperty(funcDef, includeAtom, include);
    if (init.length())
    {
        BuildCtx ctx(*this, initAtom);

        ctx.addQuoted(init.append("(wuid);"));
    }
    IHqlExpression *pluginAttr = funcDef->queryProperty(pluginAtom);
    if (pluginAttr)
    {
        StringBuffer plugin, version;
        pluginAttr->queryChild(0)->queryValue()->getStringValue(plugin);
        pluginAttr->queryChild(1)->queryValue()->getStringValue(version);
        addPlugin(plugin.str(), version.str(), false);
        if (!libname.length())
        {
            pluginAttr->queryChild(0)->queryValue()->getStringValue(libname);
            getFullFileName(libname, true);
        }
    }
    if (!funcDef->hasProperty(ctxmethodAtom) && !funcDef->hasProperty(gctxmethodAtom) && !funcDef->hasProperty(methodAtom))
    {
        if (libname.length())
            useLibrary(libname.str());
    }
    if (include.length())
        useInclude(include.str());
    return true;
}


void HqlCppInstance::useInclude(const char * include)
{
    insertUniqueString(includes, include);
}

void HqlCppInstance::useLibrary(const char * libname)
{
    insertUniqueString(modules, libname);
}

void HqlCppInstance::addHint(const char * hintXml, ICodegenContextCallback * ctxCallback)
{
    if (!hintFile)
    {
        StringBuffer hintFilename;
        if (wupathname)
            hintFilename.append(wupathname);
        else
            hintFilename.append("wu");
        hintFilename.append("_hints.xml");
        Owned<IFile> file = createIFile(hintFilename);
        Owned<IFileIO> io = file->open(IFOcreate);
        if (!io)
            return;
        hintFile.setown(createIOStream(io));
        appendHintText("<Hints>\n");

        Owned<IWUQuery> query = workunit->updateQuery();
        associateLocalFile(query, FileTypeCpp, hintFilename.str(), "Hints", 0);

        ctxCallback->registerFile(hintFilename.str(), "Hints");
    }
    appendHintText(hintXml);
}

void HqlCppInstance::appendHintText(const char * xml)
{
    hintFile->write(strlen(xml), xml);
}


unsigned HqlCppInstance::addStringResource(unsigned len, const char * body)
{
    return resources.addString(len, body);
}

void HqlCppInstance::addResource(const char * type, unsigned id, unsigned len, const void * body)
{
    resources.addNamed(type, id, len, body);
}

void HqlCppInstance::addCompressResource(const char * type, unsigned id, unsigned len, const void * data)
{
#ifdef ADD_RESOURCE_AS_CPP_COMMENT
    BuildCtx ctx(*this, includeAtom);
    StringBuffer s;
    s.append("/* ").append(type).append(".").append(id).append(":\n").append(len,(const char *)data).newline().append("*/");
    ctx.addQuoted(s);
#endif

    MemoryBuffer compressed;
    compressResource(compressed, len, data);
    addResource(type, id, compressed.length(), compressed.toByteArray());
}

void HqlCppInstance::flushHints()
{
    if (hintFile)
    {
        appendHintText("</Hints>\n");
        hintFile.clear();
    }
}

IPropertyTree * HqlCppInstance::ensureWebServiceInfo()
{
    if (!webServiceInfo)
        webServiceInfo.setown(createPTree("WebServicesInfo", false));
    return webServiceInfo;
}


void HqlCppInstance::addWebServicesResource()
{
    if (webServiceInfo)
    {
        StringBuffer webXML;
        toXML(webServiceInfo, webXML);
        addCompressResource("SOAPINFO", 1000, webXML.length(), webXML.str());
    }
}


void HqlCppInstance::addWebServices(IPropertyTree * info)
{
    if (info)
        mergePTree(ensureWebServiceInfo(), info);
}

void HqlCppInstance::flushResources(const char *filename, ICodegenContextCallback * ctxCallback)
{
    addPluginsAsResource();
    addWebServicesResource();
    if (resources.count())
    {
        bool flushText = workunit->getDebugValueBool("flushResourceAsText", false);

        StringBuffer path, trailing;
        splitFilename(filename, &path, &path, &trailing, &trailing);

        StringBuffer ln;
        ln.append(path).append(SharedObjectPrefix).append(trailing).append(LibraryExtension);
#ifdef __64BIT__
        bool target64bit = workunit->getDebugValueBool("target64bit", true);
#else
        bool target64bit = workunit->getDebugValueBool("target64bit", false);
#endif
        resources.flush(ln.str(), flushText, target64bit);

        StringBuffer resTextName;
        if (flushText && resources.queryWriteText(resTextName, ln))
        {
            Owned<IWUQuery> query = workunit->updateQuery();
            associateLocalFile(query, FileTypeHintXml, resTextName, "Workunit resource text", 0);
            ctxCallback->registerFile(resTextName, "Workunit Res.txt");
        }
        useLibrary(filename);
    }
}

IHqlCppInstance * createCppInstance(IWorkUnit *wu, const char * wupathname)
{
    return new HqlCppInstance(wu, wupathname);
}

//===========================================================================


#include "hqlcppsys.ecl"

HqlCppTranslator::HqlCppTranslator(IErrorReceiver * _errors, const char * _soName, IHqlCppInstance * _code, ClusterType _targetClusterType, ICodegenContextCallback *_ctxCallback) : ctxCallback(_ctxCallback)
{
    targetClusterType = _targetClusterType;
    {
        CriticalBlock block(*systemCS);
        if (!cppSystemScope)
        {
            StringBuffer systemText;
            unsigned size = 0;
            for (unsigned i1=0; cppSystemText[i1]; i1++)
                size += strlen(cppSystemText[i1]) + 2;
            systemText.ensureCapacity(size);
            for (unsigned i2=0; cppSystemText[i2]; i2++)
                systemText.append(cppSystemText[i2]).newline();

            MultiErrorReceiver errs;
            HqlDummyLookupContext ctx(&errs);
            cppSystemScope = createScope();
            Owned<ISourcePath> sysPath = createSourcePath("<system-definitions>");
            Owned<IFileContents> systemContents = createFileContentsFromText(systemText.str(), sysPath);
            OwnedHqlExpr query = parseQuery(cppSystemScope, systemContents, ctx, NULL, false);
            if (errs.errCount())
            {
                StringBuffer errtext;
                IECLError *first = errs.firstError();
                first->toString(errtext);
                throw MakeStringException(HQLERR_FailedToLoadSystemModule, "%s @ %d:%d", errtext.str(), first->getColumn(), first->getLine());
            } 
    #if 0
            else if (errs.warnCount())
            {
                StringBuffer s;
                errs.toString(s);
                PrintLog("Parsing system scope: ");
                PrintLog(s.str());
            }
    #endif
        }
    }
    errors = _errors;
    hints = 0;
    litno = 0;
    soName.set(_soName);
    HqlDummyLookupContext dummyctx(NULL);
    OwnedHqlExpr internalScopeLookup = cppSystemScope->lookupSymbol(createIdentifierAtom("InternalCppService"), LSFsharedOK, dummyctx);
    internalScope = internalScopeLookup->queryScope();
    _clear(options);                    // init options is called later, but depends on the workunit.
    startCursorSet = 0;
    requireTable = true;
    activeGraphCtx = NULL;
    maxSequence = 0;
    contextAvailable = true;
    graphSeqNumber = 0;
    nlpParse = NULL;
    outputLibrary = NULL;
    checkedEmbeddedCpp = false;
    cachedAllowEmbeddedCpp = true;
    checkedPipeAllowed = false;
    activitiesThisCpp = 0;
    curCppFile = 0;
    timeReporter.setown(createStdTimeReporter());
    curActivityId = 0;
    holeUniqueSequence = 0;
    nextUid = 0;
    nextTypeId = 0;
    nextFieldId = 0;
    code = (HqlCppInstance*)_code;
}

HqlCppTranslator::~HqlCppTranslator()
{
    ::Release(nlpParse);
    ::Release(outputLibrary);
}

void HqlCppTranslator::setTargetClusterType(ClusterType clusterType)
{
    targetClusterType = clusterType;
}

void HqlCppTranslator::checkAbort()
{
    if (wu() && wu()->aborting())
        throw MakeStringException(HQLERR_ErrorAlreadyReported, "Aborting");
}

// Option: (Name, value, ?overridden, default()) 
// problems:
// default value can depend on another option (e.g., cluster type/supports lcr).
// don't want code in multiple places - e.g., the values initialized and defaulted, and dependencies calculations duplicated separately.
// don't want lots of start up costs each time the translator is created -> lightweight classes if any.
// don't really want two structures, one for the definitions, and another for the values.
//RESOLVED? want to walk the debug options provided, instead of checking for each possibility in turn.   
// Without this restriction it becomes much easier.
void HqlCppTranslator::cacheOptions()
{
    SCMStringBuffer targetText;
    wu()->getDebugValue("targetClusterType", targetText);
    ClusterType clusterType = getClusterType(targetText.s.str());
    if (clusterType != NoCluster)
        setTargetClusterType(clusterType);

    //Some compound flags, which provide defaults for various other options.
    bool paranoid = getDebugFlag("paranoid", false);
    bool releaseMode = getDebugFlag("release", true);

    struct DebugOption 
    {
        typedef enum { typeByte, typeUnsigned, typeBool } OptionType;

        DebugOption (bool & _option, const char * name, bool defaultValue) : option(&_option), optName(name)
        { 
            _option = defaultValue;
            type = typeBool;
        }
        DebugOption (byte & _option, const char * name, byte defaultValue) : option(&_option), optName(name)
        { 
            _option = defaultValue;
            type = typeByte;
        }
        DebugOption (unsigned & _option, const char * name, unsigned defaultValue) : option(&_option), optName(name)
        { 
            _option = defaultValue;
            type = typeUnsigned;
        };

        void setValue(const char * val)
        {
            switch (type)
            {
            case typeBool:
                {
                    bool * b = (bool*)option;
                    *b = strToBool(val);
                    break;
                }
            case typeUnsigned:
                {
                    unsigned * u = (unsigned*)option;
                    *u = (unsigned)atoi(val);
                    break;
                }
            case typeByte:
                {
                    byte * b = (byte*)option;
                    *b = (byte)atoi(val);
                    break;
                }
            }
        }

        void *      option;
        const char * optName;
        OptionType type;
    };

    //Note this list cannot have any initial values which are dependent on other options.
    DebugOption debugOptions[] =
    {
        DebugOption(options.peephole,"peephole", true),
        DebugOption(options.foldConstantCast,"foldConstantCast", true),
        DebugOption(options.optimizeBoolReturn,"optimizeBoolReturn", true),
        DebugOption(options.checkRowOverflow,"checkRowOverflow", true),
        DebugOption(options.freezePersists,"freezePersists", false),
        DebugOption(options.maxRecordSize, "defaultMaxLengthRecord", MAX_RECORD_SIZE),
        DebugOption(options.maxInlineDepth, "maxInlineDepth", 999),         // a debugging setting...

        DebugOption(options.checkRoxieRestrictions,"checkRoxieRestrictions", true),     // a debug aid for running regression suite
        DebugOption(options.checkThorRestrictions,"checkThorRestrictions", true),       // a debug aid for running regression suite
        DebugOption(options.allowCsvWorkunitRead,"allowStoredCsvFormat", false),
        DebugOption(options.evaluateCoLocalRowInvariantInExtract,"evaluateCoLocalRowInvariantInExtract", false),
        DebugOption(options.spanMultipleCpp,"spanMultipleCpp", false),
        DebugOption(options.activitiesPerCpp, "<exception>", 0x7fffffff),
        DebugOption(options.allowInlineSpill,"allowInlineSpill", true),
        DebugOption(options.optimizeGlobalProjects,"optimizeGlobalProjects", false),
        DebugOption(options.optimizeResourcedProjects,"optimizeResourcedProjects", false),
        DebugOption(options.reduceNetworkTraffic,"aggressiveOptimizeProjects", false),
        DebugOption(options.notifyOptimizedProjects, "notifyOptimizedProjects", 0),
        DebugOption(options.optimizeProjectsPreservePersists,"optimizeProjectsPreservePersists", false),

        DebugOption(options.checkAsserts,"checkAsserts", true),
        DebugOption(options.optimizeLoopInvariant,"optimizeLoopInvariant", false),      // doesn't fully work yet! and has little effect, and messes up the alias dependencies
        DebugOption(options.defaultImplicitKeyedJoinLimit, "defaultImplicitKeyedJoinLimit", 10000),
        DebugOption(options.defaultImplicitIndexReadLimit, "defaultImplicitIndexReadLimit", 0),
        DebugOption(options.commonUpChildGraphs,"commonUpChildGraphs", true),
        DebugOption(options.detectAmbiguousSelector,"detectAmbiguousSelector", false),
        DebugOption(options.allowAmbiguousSelector,"allowAmbiguousSelector", false),
        DebugOption(options.preserveUniqueSelector,"preserveUniqueSelector", true),
#ifdef _DEBUG
        DebugOption(options.regressionTest,"regressionTest", true),
#else
        DebugOption(options.regressionTest,"regressionTest", false),
#endif
        DebugOption(options.addTimingToWorkunit, "addTimingToWorkunit", true),
        //recreating case can cause duplicate branches in weird situations.
        DebugOption(options.recreateMapFromIf,"recreateMapFromIf", !targetThor()),

        DebugOption(options.showMetaText,"debugShowMetaText", false),
        DebugOption(options.resourceSequential,"resourceSequential", false),
        DebugOption(options.workunitTemporaries,"workunitTemporaries", true),
        DebugOption(options.resourceConditionalActions,"resourceConditionalActions", false),  //targetRoxie() ??
        DebugOption(options.minimizeWorkunitTemporaries, "<exception>", false),
        DebugOption(options.pickBestEngine,"pickBestEngine", true),
        DebugOption(options.groupedChildIterators,"groupedChildIterators", false),
        DebugOption(options.noAllToLookupConversion,"noAllToLookupConversion", false),
        DebugOption(options.notifyWorkflowCse,"notifyWorkflowCse", true),
        DebugOption(options.performWorkflowCse,"performWorkflowCse", false),

        DebugOption(options.warnOnImplicitJoinLimit,"warnOnImplicitJoinLimit", targetRoxie()),
        DebugOption(options.warnOnImplicitReadLimit,"warnOnImplicitReadLimit", targetRoxie()),

        DebugOption(options.convertJoinToLookup,"convertJoinToLookup", true),
        DebugOption(options.convertJoinToLookupIfSorted,"convertJoinToLookupIfSorted", false),      // should change to false once tested
        DebugOption(options.spotCSE,"spotCSE", true),
        DebugOption(options.optimizeNonEmpty,"optimizeNonEmpty", !targetThor()),                // not sure that it will be conditional resourced correctly for thor
        DebugOption(options.allowVariableRoxieFilenames,"allowVariableRoxieFilenames", false),
        DebugOption(options.foldConstantDatasets,"foldConstantDatasets", true),
        DebugOption(options.hoistSimpleGlobal,"hoistSimpleGlobal", true),
        DebugOption(options.percolateConstants,"percolateConstants", true),
        DebugOption(options.percolateFilters,"percolateFilters", false),
        DebugOption(options.usePrefetchForAllProjects,"usePrefetchForAllProjects", false),
        DebugOption(options.allFilenamesDynamic,"allFilenamesDynamic", false),
        DebugOption(options.optimizeSteppingPostfilter,"optimizeSteppingPostfilter", true),
        DebugOption(options.moveUnconditionalActions,"moveUnconditionalActions", false),
        DebugOption(options.paranoidCheckNormalized, "paranoidCheckNormalized", paranoid),
        DebugOption(options.paranoidCheckDependencies, "paranoidCheckDependencies", paranoid),
        DebugOption(options.preventKeyedSplit,"preventKeyedSplit", true),
        DebugOption(options.preventSteppedSplit,"preventSteppedSplit", true),
        DebugOption(options.canGenerateSimpleAction,"canGenerateSimpleAction", true),
        DebugOption(options.minimizeActivityClasses,"minimizeActivityClasses", true),
        DebugOption(options.maxRootMaybeThorActions, "maxRootMaybeThorActions", 0),
        DebugOption(options.includeHelperInGraph,"includeHelperInGraph", false),
        DebugOption(options.supportsLinkedChildRows,"supportsLinkedChildRows", (targetClusterType != ThorCluster)),
        DebugOption(options.minimizeSkewBeforeSpill,"minimizeSkewBeforeSpill", false),
        DebugOption(options.createSerializeForUnknownSize,"createSerializeForUnknownSize", false),
        DebugOption(options.implicitLinkedChildRows,"implicitLinkedChildRows", false),
        DebugOption(options.tempDatasetsUseLinkedRows,"tempDatasetsUseLinkedRows", false),
//      DebugOption(options.mainRowsAreLinkCounted,"mainRowsAreLinkCounted", options.supportsLinkedChildRows),
        DebugOption(options.allowSections,"allowSections", true),
        DebugOption(options.autoPackRecords,"autoPackRecords", false),
        DebugOption(options.commonUniqueNameAttributes,"commonUniqueNameAttributes", true),
        DebugOption(options.sortIndexPayload,"sortIndexPayload", true),
        DebugOption(options.foldFilter,"foldFilter", true),
        DebugOption(options.finalizeAllRows, "finalizeAllRows", false),
        DebugOption(options.finalizeAllVariableRows, "finalizeAllVariableRows", true),
        DebugOption(options.maxStaticRowSize , "maxStaticRowSize", MAX_STATIC_ROW_SIZE),
        DebugOption(options.maxLocalRowSize , "maxLocalRowSize", MAX_LOCAL_ROW_SIZE),
        DebugOption(options.optimizeGraph,"optimizeGraph", true),
        DebugOption(options.optimizeChildGraph,"optimizeChildGraph", false),
        DebugOption(options.orderDiskFunnel,"orderDiskFunnel", true),
        DebugOption(options.alwaysAllowAllNodes,"alwaysAllowAllNodes", false),
        DebugOption(options.slidingJoins,"slidingJoins", false),
        DebugOption(options.foldOptimized,"foldOptimized", false),
        DebugOption(options.globalFold,"globalFold", true),
        DebugOption(options.globalOptimize,"globalOptimize", false),
        DebugOption(options.optimizeThorCounts,"optimizeThorCounts", true),
        DebugOption(options.applyInstantEclTransformations,"applyInstantEclTransformations", false),        // testing option
        DebugOption(options.calculateComplexity,"calculateComplexity", false),
        DebugOption(options.generateLogicalGraph,"generateLogicalGraph", false),
        DebugOption(options.generateLogicalGraphOnly,"generateLogicalGraphOnly", false),
        DebugOption(options.globalAutoHoist,"globalAutoHoist", true),

        DebugOption(options.useLinkedRawIterator,"useLinkedRawIterator", false),
        DebugOption(options.useLinkedNormalize,"useLinkedNormalize", false),

        DebugOption(options.applyInstantEclTransformationsLimit, "applyInstantEclTransformationsLimit", 100),
        DebugOption(options.insertProjectCostLevel, "insertProjectCostLevel", (unsigned)-1),
        DebugOption(options.dfaRepeatMax, "dfaRepeatMax", 10),
        DebugOption(options.dfaRepeatMaxScore, "dfaRepeatMaxScore", 100),
        DebugOption(options.debugNlp, "debugNlp", DEFAULT_NLP_DETAIL),
        DebugOption(options.regexVersion, "regexVersion",2),
        DebugOption(options.parseDfaComplexity, "parseDfaComplexity", (unsigned)-1),
        DebugOption(options.expandRepeatAnyAsDfa,"expandRepeatAnyAsDfa", true),
        DebugOption(options.resourceMaxMemory, "resourceMaxMemory", 0),
        DebugOption(options.resourceMaxSockets, "resourceMaxSockets", 0),
        DebugOption(options.resourceMaxActivities, "resourceMaxActivities", 0),
        DebugOption(options.resourceMaxHeavy, "resourceMaxHeavy", 1),
        DebugOption(options.resourceMaxDistribute, "resourceMaxDistribute", 2),
        DebugOption(options.unlimitedResources,"unlimitedResources", false),
        DebugOption(options.resourceUseMpForDistribute,"resourceUseMpForDistribute", false),
        DebugOption(options.filteredReadSpillThreshold, "filteredReadSpillThreshold", 999),
        DebugOption(options.allowThroughSpill,"allowThroughSpill", true),
        DebugOption(options.minimiseSpills,"minimiseSpills", false),
        DebugOption(options.spillMultiCondition,"spillMultiCondition", false),
        DebugOption(options.spotThroughAggregate,"spotThroughAggregate", true),
        DebugOption(options.hoistResourced,"hoistResourced", true),
        DebugOption(options.minimizeSpillSize, "minimizeSpillSize", 0),
        DebugOption(options.maximizeLexer,"maximizeLexer", false),
        DebugOption(options.foldStored,"foldStored", false),
        DebugOption(options.spotTopN,"spotTopN", true),
        DebugOption(options.topnLimit, "topnLimit", 10000),
        DebugOption(options.groupAllDistribute,"groupAllDistribute", false),
        DebugOption(options.spotLocalMerge,"spotLocalMerge", true),
        DebugOption(options.spotPotentialKeyedJoins,"spotPotentialKeyedJoins", false),
        DebugOption(options.combineTrivialStored,"combineTrivialStored", true),
        DebugOption(options.combineAllStored,"combineAllStored", false),
        DebugOption(options.allowStoredDuplicate,"allowStoredDuplicate", false),    // only here as a temporary workaround
        DebugOption(options.specifiedClusterSize, "clusterSize", 0),
        DebugOption(options.globalFoldOptions, "globalFoldOptions", (unsigned)-1),
        DebugOption(options.allowScopeMigrate,"allowScopeMigrate", true),
        DebugOption(options.supportFilterProject,"supportFilterProject", true),
        DebugOption(options.normalizeExplicitCasts,"normalizeExplicitCasts", true),
        DebugOption(options.optimizeInlineSource,"optimizeInlineSource", false),
        DebugOption(options.optimizeDiskSource,"optimizeDiskSource", true),
        DebugOption(options.optimizeIndexSource,"optimizeIndexSource", true),
        DebugOption(options.optimizeChildSource,"optimizeChildSource", false),
        DebugOption(options.createCountFile,"countFile", true),
        DebugOption(options.createCountIndex,"countIndex", false),
        DebugOption(options.reportLocations,"reportLocations", true),
        DebugOption(options.debugGeneratedCpp,"debugGeneratedCpp", false),
        DebugOption(options.addFilesnamesToGraph,"addFilesnamesToGraph", true),
        DebugOption(options.normalizeLocations,"normalizeLocations", true),
        DebugOption(options.ensureRecordsHaveSymbols,"ensureRecordsHaveSymbols", true),
        DebugOption(options.constantFoldNormalize,"constantFoldNormalize", true),
        DebugOption(options.constantFoldPostNormalize,"constantFoldPostNormalize", false),
        DebugOption(options.optimizeGrouping,"optimizeGrouping", true),
        DebugOption(options.showMetaInGraph,"showMetaInGraph", false),
        DebugOption(options.spotComplexClasses,"spotComplexClasses", true),
        DebugOption(options.complexClassesThreshold,"complexClassesThreshold", 5000),
        DebugOption(options.complexClassesActivityFilter,"complexClassesActivityFilter", 0),
        DebugOption(options.optimizeString1Compare,"optimizeString1Compare", true),
        DebugOption(options.optimizeSpillProject,"optimizeSpillProject", true),
        DebugOption(options.expressionPeephole,"expressionPeephole", false),
        DebugOption(options.optimizeIncrement,"optimizeIncrement", true),
        DebugOption(options.supportsMergeDistribute,"supportsMergeDistribute", true),
        DebugOption(options.debugNlpAsHint,"debugNlpAsHint", false),
        DebugOption(options.forceVariableWuid,"forceVariableWuid", false),
        DebugOption(options.okToDeclareAndAssign,"okToDeclareAndAssign", false),
        DebugOption(options.noteRecordSizeInGraph,"noteRecordSizeInGraph", true),
        DebugOption(options.convertRealAssignToMemcpy,"convertRealAssignToMemcpy", false),
        DebugOption(options.allowActivityForKeyedJoin,"allowActivityForKeyedJoin", false),
        DebugOption(options.forceActivityForKeyedJoin,"forceActivityForKeyedJoin", false),
        DebugOption(options.addLibraryInputsToGraph,"addLibraryInputsToGraph", false),
        DebugOption(options.showRecordCountInGraph,"showRecordCountInGraph", true),
        DebugOption(options.serializeRowsetInExtract,"serializeRowsetInExtract", false),
        DebugOption(options.optimizeInSegmentMonitor,"optimizeInSegmentMonitor", true),
        DebugOption(options.supportDynamicRows,"supportDynamicRows", true),
        DebugOption(options.testIgnoreMaxLength,"testIgnoreMaxLength", false),
        DebugOption(options.limitMaxLength,"limitMaxLength", false),
        DebugOption(options.trackDuplicateActivities,"trackDuplicateActivities", false),
        DebugOption(options.showActivitySizeInGraph,"showActivitySizeInGraph", false),
        DebugOption(options.addLocationToCpp,"addLocationToCpp", false),
        DebugOption(options.alwaysCreateRowBuilder,"alwaysCreateRowBuilder", false),
        DebugOption(options.precalculateFieldOffsets,"precalculateFieldOffsets", false),
        DebugOption(options.generateStaticInlineTables,"generateStaticInlineTables", true),
        DebugOption(options.staticRowsUseStringInitializer,"staticRowsUseStringInitializer", true),
        DebugOption(options.convertWhenExecutedToCompound,"convertWhenExecutedToCompound", queryLegacyEclSemantics()),
        DebugOption(options.standAloneExe,"standAloneExe", false),
        DebugOption(options.enableCompoundCsvRead,"enableCompoundCsvRead", true),
    };

    //get options values from workunit
    const unsigned numDebugOptions = _elements_in(debugOptions);
    Owned<IStringIterator> debugs(&wu()->getDebugValues());
    SCMStringBuffer name, val;
    ForEach(*debugs)
    {
        debugs->str(name);
        wu()->getDebugValue(name.str(),val);

        unsigned x = 0;
        for (; x < numDebugOptions; x++)
        {
            if (0 == stricmp(name.str(), debugOptions[x].optName))
            {
                debugOptions[x].setValue(val.str());
                break;
            }
        }
    }

    //The following cases handle options whose default values are dependent on other options.  
    //Or where one debug options sets more than one option
    options.hasResourceUseMpForDistribute = wu()->hasDebugValue("resourceUseMpForDistribute");
    if (!options.supportsLinkedChildRows) 
        options.implicitLinkedChildRows = false;

    if (options.spanMultipleCpp)
    {
        options.activitiesPerCpp = wu()->getDebugValueInt("activitiesPerCpp", DEFAULT_ACTIVITIES_PER_CPP);
        curCppFile = 1;
    }

    options.targetCompiler = DEFAULT_COMPILER;
    if (wu()->hasDebugValue("targetGcc"))
        options.targetCompiler = wu()->getDebugValueBool("targetGcc", false) ? GccCppCompiler : Vs6CppCompiler;

    SCMStringBuffer compilerText;
    wu()->getDebugValue("targetCompiler", compilerText);
    for (CompilerType iComp = (CompilerType)0; iComp < MaxCompiler; iComp = (CompilerType)(iComp+1))
    {
        if (stricmp(compilerText.s.str(), compilerTypeText[iComp]) == 0)
            options.targetCompiler = iComp;
    }

    if (getDebugFlag("optimizeProjects", true))
    {
        options.optimizeGlobalProjects = true;
        options.optimizeResourcedProjects = true;
    }

    options.mainRowsAreLinkCounted = options.supportsLinkedChildRows && getDebugFlag("mainRowsAreLinkCounted", true);
    options.minimizeWorkunitTemporaries = !options.workunitTemporaries || getDebugFlag("minimizeWorkunitTemporaries", false);//options.resourceConditionalActions);
    options.tempDatasetsUseLinkedRows = getDebugFlag("tempDatasetsUseLinkedRows", options.implicitLinkedChildRows);

    options.inlineStringThreshold = wu()->getDebugValueInt("inlineStringThreshold", (options.targetCompiler != Vs6CppCompiler) ? 0 : 10000);

    if (!options.globalFold)
    {
        options.constantFoldNormalize = false;
        options.constantFoldPostNormalize = false;
    }

    //A meta flag for enabling link counted child rows.
    bool useLCR = getDebugFlag("linkCountedRows", getDebugFlag("testLCR", true));
    if (options.supportsLinkedChildRows && useLCR)
    {
        options.implicitLinkedChildRows = true;
        options.tempDatasetsUseLinkedRows = true;
        options.useLinkedRawIterator = true;
        options.useLinkedNormalize = true;
        options.finalizeAllRows = true;     // inline temporary rows should actually be ok.
        options.finalizeAllVariableRows = true;
    }

    postProcessOptions();
}

void HqlCppTranslator::postProcessOptions()
{
//Any post processing - e.g., dependent flags goes here...
    if (targetClusterType == ThorCluster)
        options.supportDynamicRows = false;

    if (options.supportDynamicRows)
    {
        options.finalizeAllVariableRows = true;
    }
    else
    {
        options.testIgnoreMaxLength = false;
        options.limitMaxLength = false;
    }

    if (options.supportsLinkedChildRows) 
    {
        options.maxStaticRowSize = 0;
    }
    else
    {
        options.maxStaticRowSize = (unsigned)-1;
        options.implicitLinkedChildRows = false;
        options.tempDatasetsUseLinkedRows = false;
        options.useLinkedRawIterator = false;
        options.useLinkedNormalize = false;
        options.finalizeAllRows = false;
        options.finalizeAllVariableRows = false;
    }

    options.optimizeDiskFlag = 0;
    if (options.optimizeInlineSource) 
        options.optimizeDiskFlag |= CSFnewinline;
    if (options.optimizeDiskSource)
        options.optimizeDiskFlag |= CSFnewdisk;
    if (options.optimizeIndexSource)
        options.optimizeDiskFlag |= CSFnewindex;
    if (options.optimizeChildSource)
        options.optimizeDiskFlag |= CSFnewchild;

    if (options.optimizeIndexSource && (targetClusterType != ThorCluster))          // thor doesn't support child queries - so still need following
        options.createCountIndex = false;

    if (targetThor())
    {
        //options.resourceConditionalActions = false;
        //options.resourceSequential = false;
    }

    if (!targetThor())
    {
        //Roxie doesn't gain from additional projects, hthor doesn't support split
        options.optimizeSpillProject = false;
    }

    if (options.resourceSequential)
        options.resourceConditionalActions = true;

    //Probably best to ignore this warning. - possibly configure it based on some other option
    queryWarningProcessor().addGlobalOnWarning(HQLWRN_FoldRemoveKeyed, ignoreAtom);

    if (options.forceVariableWuid)
        wu()->setCloneable(true);
}

unsigned HqlCppTranslator::getOptimizeFlags() const
{
    unsigned optFlags = HOOfold;
    switch (targetClusterType)
    {
    case RoxieCluster:
        optFlags |= HOOnoclonelimit;
        break;
    case HThorCluster:
        optFlags |= HOOnocloneindexlimit;
        break;
    case ThorCluster:
    case ThorLCRCluster:
        break;
    default:
        UNIMPLEMENTED;
    }
    if ((options.optimizeDiskFlag & CSFnewdisk) && (options.optimizeDiskFlag & CSFnewindex))
        optFlags |= HOOhascompoundaggregate;
    if (options.foldConstantDatasets)
        optFlags |= HOOfoldconstantdatasets;
    return optFlags;
}


void HqlCppTranslator::overrideOptionsForLibrary()
{
    options.workunitTemporaries = false;
    options.pickBestEngine = false;
    options.minimizeWorkunitTemporaries = true;
}


void HqlCppTranslator::overrideOptionsForQuery()
{
    options.workunitTemporaries = getDebugFlag("workunitTemporaries", true);
    options.pickBestEngine = getDebugFlag("pickBestEngine", true);
    options.minimizeWorkunitTemporaries = !options.workunitTemporaries || getDebugFlag("minimizeWorkunitTemporaries", false);//options.resourceConditionalActions);
}

IHqlExpression *HqlCppTranslator::addBigLiteral(const char *lit, unsigned litLen)
{
    unsigned resid = code->addStringResource(litLen, lit);
    HqlExprArray args;
    args.append(*getSizetConstant(resid));
    return bindTranslatedFunctionCall(loadResourceAtom, args);
}

IHqlExpression * HqlCppTranslator::addLiteral(const char * text)
{
    return createConstant(text);
}

IHqlExpression *HqlCppTranslator::addDataLiteral(const char *lit, unsigned litLen)
{
    if (!canGenerateStringInline(litLen))
        return addBigLiteral(lit, litLen);
    else
        return createConstant(createStringValue(lit, litLen));
}


IHqlExpression *HqlCppTranslator::addStringLiteral(const char *lit)
{
    unsigned litLen = strlen(lit);
    if (!canGenerateStringInline(litLen))
        return addBigLiteral(lit, litLen+1);
    else
        return createConstant(createStringValue(lit, litLen));
}

bool HqlCppTranslator::allowEmbeddedCpp()
{
    if (!checkedEmbeddedCpp)
    {
        cachedAllowEmbeddedCpp = ctxCallback->allowAccess("cpp");
        checkedEmbeddedCpp = true;
    }
    return cachedAllowEmbeddedCpp;
}

void HqlCppTranslator::checkPipeAllowed()
{
    if (!checkedPipeAllowed)
    {
        if (!ctxCallback->allowAccess("pipe"))
            throwError(HQLERR_PipeNotAllowed);
        checkedPipeAllowed = true;
    }
}

IHqlExpression * HqlCppTranslator::bindFunctionCall(_ATOM name, HqlExprArray & args)
{
    OwnedHqlExpr function = needFunction(name);
    assertex(function);
    return bindFunctionCall(function, args);
}

IHqlExpression * HqlCppTranslator::bindFunctionCall(_ATOM name, IHqlExpression * arg1)
{
    HqlExprArray args;
    args.append(*arg1);
    return bindFunctionCall(name, args);
}

IHqlExpression * HqlCppTranslator::bindFunctionCall(IHqlExpression * function, HqlExprArray & args)
{
    useFunction(function);
    IHqlExpression * ret = createBoundFunction(NULL, function, args, NULL, false);
    assertex(ret->queryExternalDefinition());
    args.kill();
    return ret;
}

IHqlExpression * HqlCppTranslator::bindFunctionCall(_ATOM name, HqlExprArray & args, ITypeInfo * newType)
{
    if (!newType)
        return bindFunctionCall(name, args);

    OwnedHqlExpr function = needFunction(name);
    useFunction(function);
    assertex(function->getOperator() == no_funcdef);
    IHqlExpression * body = function->queryChild(0);

    HqlExprArray bodyArgs;
    unwindChildren(bodyArgs, body);

    HqlExprArray funcArgs;
    funcArgs.append(*createValue(body->getOperator(), LINK(newType), bodyArgs));
    unwindChildren(funcArgs, function, 1);
    ITypeInfo * funcType = makeFunctionType(LINK(newType), LINK(function->queryChild(1)), LINK(function->queryChild(2)));
    OwnedHqlExpr newFunction = createValue(function->getOperator(), funcType, funcArgs);
    return bindFunctionCall(newFunction, args);
}

IHqlExpression * HqlCppTranslator::bindTranslatedFunctionCall(IHqlExpression * function, HqlExprArray & args)
{
    useFunction(function);
    IHqlExpression * ret = createTranslatedExternalCall(NULL, function, args);
    assertex(ret->queryExternalDefinition());
    args.kill();
    return ret;
}


IHqlExpression * HqlCppTranslator::bindTranslatedFunctionCall(_ATOM name, HqlExprArray & args)
{
    OwnedHqlExpr function = needFunction(name);
    return bindTranslatedFunctionCall(function, args);
}

void HqlCppTranslator::buildTranslatedFunctionCall(BuildCtx & ctx, _ATOM name, HqlExprArray & args)
{
    OwnedHqlExpr call = bindTranslatedFunctionCall(name, args);
    ctx.addExpr(call);
}


void HqlCppTranslator::buildFunctionCall(BuildCtx & ctx, _ATOM name, HqlExprArray & args)
{
    OwnedHqlExpr call = bindFunctionCall(name, args);
    buildStmt(ctx, call);
}


/* Args: all elements in it are LINKED */
void HqlCppTranslator::callProcedure(BuildCtx & ctx, _ATOM name, HqlExprArray & args)
{
    OwnedHqlExpr call = bindTranslatedFunctionCall(name, args);
    assertex(call->queryExternalDefinition());
    ctx.addExpr(call);
}

void HqlCppTranslator::buildServicePrototypes(IHqlScope * scope)
{
    BuildCtx ctx(*code, prototypeAtom);

    StringBuffer s;
    HqlExprArray exprs;
    scope->getSymbols(exprs);
    ForEachItemIn(idx, exprs)
    {
        IHqlExpression & cur = exprs.item(idx);
        node_operator op = cur.getOperator();
        if (op == no_scope)
        {
            s.clear().append("/* SERVICE ").append(cur.queryName()).append(" */");
            ctx.addQuoted(s);

            IHqlScope * serviceScope = cur.queryScope();
            HqlExprArray funcs;
            serviceScope->getSymbols(funcs);
            ForEachItemIn(idx2, funcs)
            {
                IHqlExpression & curFunc = funcs.item(idx2);
                IHqlExpression * def = curFunc.queryExternalDefinition();
                if (def)
                {
                    s.clear().append("//").append(cur.queryName()).append(".").append(curFunc.queryName());
                    ctx.addQuoted(s);
                    expandFunctionPrototype(ctx, def, true);
                }
            }
        }
    }
}


bool HqlCppTranslator::getDebugFlag(const char * name, bool defValue)
{
    return wu()->getDebugValueBool(name, defValue);
}

void HqlCppTranslator::doReportWarning(IHqlExpression * location, unsigned id, const char * msg)
{
    Owned<IECLError> warnError;
    if (!location)
        location = queryActiveActivityLocation();
    if (location)
        warnError.setown(createECLError(false, id, msg, location->querySourcePath()->str(), location->getStartLine(), location->getStartColumn(), 0));
    else
        warnError.setown(createECLError(false, id, msg, NULL, 0, 0, 0));

    warningProcessor.report(NULL, errors, warnError);
}

void HqlCppTranslator::reportWarning(IHqlExpression * location, unsigned id, const char * msg, ...)
{
    StringBuffer s;
    va_list args;
    va_start(args, msg);
    s.valist_appendf(msg, args);
    va_end(args);
    doReportWarning(location, id, s.str());
}

void HqlCppTranslator::reportWarning(unsigned id, const char * msg, ...)
{
    StringBuffer s;
    va_list args;
    va_start(args, msg);
    s.valist_appendf(msg, args);
    va_end(args);
    doReportWarning(NULL, id, s.str());
}

void HqlCppTranslator::addWorkunitException(WUExceptionSeverity severity, unsigned code, const char * text, IHqlExpression * location)
{
    Owned<IWUException> msg = wu()->createException();
    msg->setExceptionSource("Code Generator");
    if (code)
        msg->setExceptionCode(code);
    msg->setExceptionMessage(text);
    msg->setSeverity(severity);
    msg->setTimeStamp(NULL);

    if (!location)
        location = queryActiveActivityLocation();
    if (location)
    {
        msg->setExceptionFileName(location->querySourcePath()->str());
        msg->setExceptionLineNo(location->getStartLine());
        msg->setExceptionColumn(location->getStartColumn());
    }
}


IHqlExpression * HqlCppTranslator::queryActiveNamedActivity()
{
    ForEachItemInRev(i, activityExprStack)
    {
        IHqlExpression & cur = activityExprStack.item(i);
        IHqlExpression * symbol = queryNamedSymbol(&cur);
        if (symbol && symbol->querySourcePath())
            return symbol;
        if (isCompoundSource(&cur))
        {
            IHqlExpression * child = cur.queryChild(0);
            if (hasNamedSymbol(child))
                return child;
        }
    }
    return NULL;
}

IHqlExpression * HqlCppTranslator::queryActiveActivityLocation() const
{
    ForEachItemInRev(i, activityExprStack)
    {
        IHqlExpression & cur = activityExprStack.item(i);
        IHqlExpression * location = queryLocation(&cur);
        if (location)
            return location;
        if (isCompoundSource(&cur))
        {
            location = queryLocation(cur.queryChild(0));
            if (location)
                return location;
        }
    }
    return NULL;
}

void HqlCppTranslator::ThrowStringException(int code,const char *format, ...)
{
    IHqlExpression * location = queryActiveActivityLocation();
    if (errors && location)
    {
        StringBuffer errorMsg;
        va_list args;
        va_start(args, format);
        errorMsg.valist_appendf(format, args);
        va_end(args);
        throw createECLError(code, errorMsg.str(), location->querySourcePath()->str(), location->getStartLine(), location->getStartColumn(), 0);
    }

    va_list args;
    va_start(args, format);
    IException *ret = MakeStringExceptionVA(code, format, args);
    va_end(args);
    throw ret;
}

void HqlCppTranslator::reportErrorDirect(IHqlExpression * location, int code,const char *msg, bool alwaysAbort)
{
    if (location)
    {
        ECLlocation loc;
        loc.extractLocationAttr(location);
        if (alwaysAbort)
            throw createECLError(code, msg, loc.sourcePath->str(), loc.lineno, loc.column, loc.position);
        errors->reportError(code, msg, loc.sourcePath->str(), loc.lineno, loc.column, loc.position);
    }
    else
        throw MakeStringExceptionDirect(code, msg);
}

void HqlCppTranslator::reportError(IHqlExpression * location, int code,const char *format, ...)
{
    StringBuffer errorMsg;
    va_list args;
    va_start(args, format);
    errorMsg.valist_appendf(format, args);
    va_end(args);

    reportErrorDirect(location, code, errorMsg.str(), true);
}

//---------------------------------------------------------------------------

void HqlCppTranslator::doBuildStmtAssign(BuildCtx & ctx, IHqlExpression * target, IHqlExpression * expr)
{
    buildAssign(ctx, target, expr);
}

void HqlCppTranslator::buildAddress(BuildCtx & ctx, IHqlExpression * expr, CHqlBoundExpr & tgt)
{
    node_operator op = expr->getOperator();

    switch (op)
    {
    case no_address:
        buildExpr(ctx, expr->queryChild(0), tgt);
        break;
    case no_typetransfer:
        buildAddress(ctx, expr->queryChild(0), tgt);
        break;
    default:
        {
            Owned<IReferenceSelector> selector = buildReference(ctx, expr);
            selector->buildAddress(ctx, tgt);
            break;
        }
    }
}

bool HqlCppTranslator::hasAddress(BuildCtx & ctx, IHqlExpression * expr)
{
    switch (expr->getOperator())
    {
    case no_deref:
    case no_variable:
        return true;
    case no_field:
    case no_select:
        {
            Owned<IReferenceSelector> selector = buildReference(ctx, expr);
            return !selector->isConditional();
        }
    case no_typetransfer:
        return hasAddress(ctx, expr->queryChild(0));
    default:
        return false;
    }
}

void HqlCppTranslator::buildAssign(BuildCtx & ctx, IHqlExpression * target, IHqlExpression * expr)
{
#ifdef ADD_ASSIGNMENT_COMMENTS
    if (target->getOperator() == no_select)
    {
        StringBuffer s;
        ctx.addQuoted(s.append("//Assign to field ").append(target->queryChild(1)->queryName()));
    }
#endif

    Owned<IReferenceSelector> selector = buildReference(ctx, target);
    if (expr->getOperator() == no_null)
        selector->buildClear(ctx, 0);   
    else if (target->isDatarow() && (!hasReferenceModifier(target->queryType()) || !recordTypesMatch(target->queryType(), expr->queryType())))
        buildRowAssign(ctx, selector, expr);
    else
        selector->set(ctx, expr);
}

void HqlCppTranslator::doBuildStmtAssignModify(BuildCtx & ctx, IHqlExpression * target, IHqlExpression * expr, node_operator assignOp)
{
    Owned<IReferenceSelector> selector = buildReference(ctx, target);
    selector->modifyOp(ctx, expr, assignOp);
}

void HqlCppTranslator::buildExprAssign(BuildCtx & ctx, const CHqlBoundTarget & target, IHqlExpression * expr)
{
    /*
    switch (target.queryType().getTypeCode())
    {
    case type_table:
    case type_groupedtable:
        buildDatasetAssign(ctx, target, expr);
        break;
    case type_row:
        buildAssignRow(ctx, target, expr);
        break;
    }
    */

    node_operator op = expr->getOperator();
    switch (op)
    {
    case no_constant:
        if (!isNullAssign(target, expr))
            doBuildExprAssign(ctx, target, expr);
        else
            ctx.addAssign(target.length, queryZero());
        break;
    case no_regex_find:
    case no_regex_replace:
        doBuildAssignRegexFindReplace(ctx, target, expr);
        break;
    case no_matched:
    case no_matchtext:
    case no_matchlength:
    case no_matchposition:
    case no_matchunicode:
    case no_matchutf8:
        doBuildMatched(ctx, &target, expr, NULL);
        break;
    case no_matchattr:
        doBuildMatchAttr(ctx, &target, expr, NULL);
        break;
    case no_loopcounter:
        doBuildAssignLoopCounter(ctx, target, expr);
        break;
    case no_evalonce:
        doBuildEvalOnce(ctx, &target, expr, NULL);
        break;
    case no_alias_scope:
        {
            expandAliasScope(ctx, expr);
            buildExprAssign(ctx, target, expr->queryChild(0));
            break;
        }
    case no_case:
    case no_map:
        {
            HqlCppCaseInfo info(*this);
            doBuildCaseInfo(expr, info);
            info.buildAssign(ctx, target);
            break;
        }
    case no_which:
    case no_rejected:
        doBuildAssignWhich(ctx, target, expr);
        break;
    case no_call:
    case no_externalcall:
        doBuildAssignCall(ctx, target, expr);
        break;
    case no_cast:
    case no_implicitcast:
        doBuildAssignCast(ctx, target, expr);
        break;
    case no_choose:
        doBuildAssignChoose(ctx, target, expr);
        break;
    case no_comma:
    case no_compound:
        buildStmt(ctx, expr->queryChild(0));
        buildExprAssign(ctx, target, expr->queryChild(1));
        break;
    case no_concat:
        doBuildAssignConcat(ctx, target, expr);
        break;
    case no_div:
    case no_modulus:
        doBuildAssignDivide(ctx, target, expr);
        break;
    case no_crc:
    case no_hash:
    case no_hash32:
    case no_hash64:
        doBuildAssignHashCrc(ctx, target, expr);
        break;
    case no_hashmd5:
        doBuildAssignHashMd5(ctx, target, expr);
        break;
    case no_if:
        doBuildAssignIf(ctx, target, expr);
        break;
    case no_index:
        doBuildAssignIndex(ctx, target, expr);
        break;
    case no_in:
    case no_notin:
        {
            OwnedHqlExpr optimized = querySimplifyInExpr(expr);
            if (options.globalFold && optimized)
            {
                OwnedHqlExpr folded = foldHqlExpression(optimized);
                buildExprAssign(ctx, target, folded);
            }
            else
                doBuildAssignIn(ctx, target, expr);
            break;
        }
    case no_intformat:
        doBuildAssignFormat(intFormatAtom, ctx, target, expr);
        break;
    case no_nofold:
    case no_nohoist:
    case no_section:
    case no_sectioninput:
        buildExprAssign(ctx, target, expr->queryChild(0));
        break;
    case no_realformat:
        doBuildAssignFormat(realFormatAtom, ctx, target, expr);
        break;
    case no_order:
        doBuildAssignOrder(ctx, target, expr);
        break;
    case no_unicodeorder:
        doBuildAssignUnicodeOrder(ctx, target, expr);
        break;
    case no_substring:
        doBuildAssignSubString(ctx, target, expr);
        break;
    case no_trim:
        doBuildAssignTrim(ctx, target, expr);
        break;
    case no_field:
        throwUnexpected();
    case no_select:
        {
            OwnedHqlExpr aggregate = convertToSimpleAggregate(expr);
            if (aggregate && canProcessInline(&ctx, aggregate->queryChild(0)))
            {
                buildExprAssign(ctx, target, aggregate);
                return;
            }
            Owned<IReferenceSelector> selector = buildReference(ctx, expr);
            selector->assignTo(ctx, target);
            return;
        }
        break;
    case no_not:
        {
            IHqlExpression * child = expr->queryChild(0);
            node_operator childOp = child->getOperator();
            if (((childOp == no_and) || (childOp == no_or)) && requiresTempAfterFirst(ctx, child))
            {
                if (childOp == no_and)
                    doBuildAssignAnd(ctx, target, child, true);
                else
                {
                    OwnedHqlExpr inverted = convertOrToAnd(expr);
                    buildExprAssign(ctx, target, inverted);
                }
            }
            else
                doBuildExprAssign(ctx, target, expr);
            break;
        }
    case no_or:
        {
            IHqlExpression * left = expr->queryChild(0);
            //in always goes via an assign, so do this first, and then filter on result.
            if (left->getOperator() == no_in)
            {
                BuildCtx subctx(ctx);
                buildExprAssign(subctx, target, left);
                OwnedHqlExpr inverse = getInverse(target.expr);
                subctx.addFilter(inverse);
                buildExprAssign(subctx, target, expr->queryChild(1));
            }
            else if (requiresTempAfterFirst(ctx, expr))
            {
                OwnedHqlExpr inverted = convertOrToAnd(expr);
                buildExprAssign(ctx, target, inverted);
            }
            else
                doBuildExprAssign(ctx, target, expr);
            break;
        }
    case no_and:
        if (requiresTempAfterFirst(ctx, expr))
            doBuildAssignAnd(ctx, target, expr, false);
        else
            doBuildExprAssign(ctx, target, expr);
        break;
    case no_fromunicode:
    case no_tounicode:
        doBuildAssignToFromUnicode(ctx, target, expr);
        break;
    case no_toxml:
        doBuildAssignToXml(ctx, target, expr);
        break;
    case no_wuid:
        doBuildAssignWuid(ctx, target, expr);
        break;
    case no_xmldecode:
    case no_xmlencode:
        doBuildXmlEncode(ctx, &target, expr, NULL);
        break;
    case no_all:
        doBuildAssignAll(ctx, target, expr);
        return;
    case no_list:
        doBuildAssignList(ctx, target, expr);
        return;
    case no_addsets:
        doBuildAssignAddSets(ctx, target, expr);
        return;
    case no_createset:
        buildSetAssignViaBuilder(ctx, target, expr);
        return;
    case no_failmessage:
        doBuildAssignFailMessage(ctx, target, expr);
        return;
    case no_eventname:
        doBuildAssignEventName(ctx, target, expr);
        return;
    case no_eventextra:
        doBuildAssignEventExtra(ctx, target, expr);
        return;
    case no_catch:
        doBuildAssignCatch(ctx, target, expr);
        break;
    case no_id2blob:
        doBuildAssignIdToBlob(ctx, target, expr);
        break;
    case no_getresult:
    case no_workunit_dataset:
        if (isSameFullyUnqualifiedType(expr->queryType(), target.queryType()))
            doBuildAssignGetResult(ctx, target, expr);
        else
            doBuildExprAssign(ctx, target, expr);
        break;
    case no_getgraphresult:
        doBuildAssignGetGraphResult(ctx, target, expr);
        break;
    case no_existslist:
        doBuildAggregateList(ctx, &target, expr, NULL);
        break;
    case no_countlist:
        doBuildAggregateList(ctx, &target, expr, NULL);
        break;
    case no_sumlist:
        doBuildAggregateList(ctx, &target, expr, NULL);
        break;
    case no_minlist:
        doBuildAggregateList(ctx, &target, expr, NULL);
        break;
    case no_maxlist:
        doBuildAggregateList(ctx, &target, expr, NULL);
        break;
    case no_skip:
        {
            bool canReachFollowing = false;
            doBuildStmtSkip(ctx, expr, &canReachFollowing);
            if (canReachFollowing)
            {
                OwnedHqlExpr null = createNullExpr(expr);
                doBuildExprAssign(ctx, target, null);
            }
            break;
        }
    case no_count:
    case no_max:
    case no_min:
    case no_sum:
    case no_exists:
    case no_notexists:
        doBuildAssignAggregate(ctx, target, expr);
        break;
    case no_getenv:
        {
            OwnedHqlExpr mapped = cvtGetEnvToCall(expr);
            buildExprAssign(ctx, target, mapped);
            break;
        }
    default:
        doBuildExprAssign(ctx, target, expr);
        break;
    }
}

void HqlCppTranslator::buildExprAssignViaType(BuildCtx & ctx, const CHqlBoundTarget & target, IHqlExpression * expr, ITypeInfo * type)
{
    OwnedHqlExpr temp = createValue(no_implicitcast, LINK(type), LINK(expr));
    buildExprAssign(ctx, target, temp);
}

void HqlCppTranslator::buildExprAssignViaString(BuildCtx & ctx, const CHqlBoundTarget & target, IHqlExpression * expr, unsigned len)
{
    OwnedITypeInfo type = makeStringType(len, NULL, NULL);
    buildExprAssignViaType(ctx, target, expr, type);
}

void HqlCppTranslator::buildAssignToTemp(BuildCtx & ctx, IHqlExpression * variable, IHqlExpression * expr)
{
    CHqlBoundTarget boundTarget;
    boundTarget.expr.set(variable);
    buildExprAssign(ctx, boundTarget, expr);
}

void HqlCppTranslator::buildAssignViaTemp(BuildCtx & ctx, const CHqlBoundTarget & target, IHqlExpression * expr)
{
    CHqlBoundExpr temp;
    buildTempExpr(ctx, expr, temp);
    buildExprAssign(ctx, target, temp.expr);
}


static bool canOptimizeIncrementAssign(ITypeInfo * type)
{
    switch (type->getTypeCode())
    {
    case type_real:
        return true;
    case type_int:
        switch (type->getSize())
        {
        case 1:
        case 2:
        case 4:
        case 8:
            return true;
        }
    }
    return false;
}


IHqlExpression * HqlCppTranslator::optimizeIncrementAssign(BuildCtx & ctx, IHqlExpression * value)
{
    //MORE: Could spot x += if(cond, y, 0) and convert to if (cond) x+= y;   (especially if y is 1)
    if (value->getOperator() == no_if)
    {
        IHqlExpression * left = value->queryChild(1);
        IHqlExpression * right = value->queryChild(2);
        if (isZero(right))
        {
            buildFilter(ctx, value->queryChild(0));
            return optimizeIncrementAssign(ctx, left);
        }
        if (isZero(left))
        {
            OwnedHqlExpr filter = getInverse(value->queryChild(0));
            buildFilter(ctx, filter);
            return optimizeIncrementAssign(ctx, right);
        }
    }

    if (isCast(value))
    {
        IHqlExpression * uncast = value->queryChild(0);
        OwnedHqlExpr optimizedValue = optimizeIncrementAssign(ctx, uncast);
        if (optimizedValue != uncast)
            return ensureExprType(optimizedValue, value->queryType());
    }

    return LINK(value);
}


void HqlCppTranslator::buildIncrementAssign(BuildCtx & ctx, IHqlExpression * target, IHqlExpression * value)
{
//  CHqlBoundExpr bound;
//  buildExpr(ctx, target, bound);
    ITypeInfo * type = target->queryType();
    OwnedHqlExpr castValue = ensureExprType(value, type);

    BuildCtx condctx(ctx);
    if (options.optimizeIncrement)
    {
        castValue.setown(optimizeIncrementAssign(condctx, castValue));
        if (isZero(castValue))
            return;

        if (canOptimizeIncrementAssign(type))
        {
            CHqlBoundExpr boundTarget;
            buildExpr(condctx, target, boundTarget);        // Not very clean!

            CHqlBoundExpr boundValue;
            buildExpr(condctx, castValue, boundValue);
            condctx.addAssignIncrement(boundTarget.expr, boundValue.expr);
            return;
        }
    }

    OwnedHqlExpr plus = createValue(no_add, LINK(target), castValue.getClear());
    buildAssign(condctx, target, plus);
}

void HqlCppTranslator::buildIncrementAssign(BuildCtx & ctx, IReferenceSelector * target, IHqlExpression * value)
{
    buildIncrementAssign(ctx, target->queryExpr(), value);
//  OwnedHqlExpr plus = createValue(no_add, LINK(type), ensureExprType(target->queryExpr(), type), ensureExprType(value, type));
//  target->set(condctx, plus);
}

void HqlCppTranslator::buildIncrementAssign(BuildCtx & ctx, const CHqlBoundTarget & target, IHqlExpression * value)
{
    ITypeInfo * type = target.queryType();
    OwnedHqlExpr castValue = ensureExprType(value, type);

    BuildCtx condctx(ctx);
    if (options.optimizeIncrement)
    {
        castValue.setown(optimizeIncrementAssign(condctx, castValue));
        if (isZero(castValue))
            return;

        if (canOptimizeIncrementAssign(type))
        {
            CHqlBoundExpr boundValue;
            buildExpr(ctx, castValue, boundValue);
            ctx.addAssignIncrement(target.expr, boundValue.expr);
            return;
        }
    }

    OwnedHqlExpr plus = createValue(no_add, target.getTranslatedExpr(), castValue.getClear());
    buildExprAssign(ctx, target, plus);
}

void HqlCppTranslator::buildClear(BuildCtx & ctx, IHqlExpression * expr)
{
    OwnedHqlExpr null = createNullExpr(expr);
    buildAssign(ctx, expr, null);
}

void HqlCppTranslator::buildClear(BuildCtx & ctx, const CHqlBoundTarget & target)
{
    if (target.length)
    {
        buildAssignToTemp(ctx, target.length, queryZero());
        //NB: Don't need to clear target.pointer/target.variable if a length is defined......
        return;
    }

    if (target.isFixedSize())
    {
        OwnedHqlExpr null = createNullExpr(target.expr);
        buildExprAssign(ctx, target, null);
    }
    else
    {
        StringBuffer code;
        if (hasWrapperModifier(target.queryType()))
            generateExprCpp(code, target.expr).append(".clear();");
        else
            generateExprCpp(code, target.expr).append(" = 0;");

        ctx.addQuoted(code);
    }
}

void HqlCppTranslator::buildFilter(BuildCtx & ctx, IHqlExpression * expr)
{
    node_operator op = expr->getOperator();

    switch (op)
    {
    case no_attr:
    case no_attr_expr:
    case no_attr_link:
        return;
    case no_and:
        doBuildFilterAnd(ctx, expr);
        return;
    case no_not:
        {
            IHqlExpression * child = expr->queryChild(0);
            if ((child->getOperator() == no_or) && requiresTempAfterFirst(ctx, child))
            {
                OwnedHqlExpr inverted = convertOrToAnd(expr);
                buildFilter(ctx,inverted);
                return;
            }
        }
        break;
    case no_or:
        if (!(hints & HintSize) && requiresTempAfterFirst(ctx, expr))
        {
            OwnedHqlExpr inverted = convertOrToAnd(expr);
            buildFilter(ctx, inverted);
            return;
        }
        break;
    case no_between:
    case no_notbetween:
        {
            OwnedHqlExpr between = expandBetween(expr);
            buildFilter(ctx, between);
            return;
        }
    case no_alias_scope:
        {
            expandAliasScope(ctx, expr);
            buildFilter(ctx, expr->queryChild(0));
            return;
        }
    }
    buildFilterViaExpr(ctx, expr);
}

IHqlStmt * HqlCppTranslator::buildFilterViaExpr(BuildCtx & ctx, IHqlExpression * expr)
{
    //default action...
    CHqlBoundExpr pure;
    buildExpr(ctx, expr, pure);
    if (pure.length)                        // check length is non zero
        return ctx.addFilter(pure.length);
    else
    {
        IHqlStmt * stmt =  ctx.addFilter(pure.expr);
        ctx.associateExpr(expr, queryBoolExpr(true));
        return stmt;
    }
}

void HqlCppTranslator::tidyupExpr(BuildCtx & ctx, CHqlBoundExpr & bound)
{
    if (isPushed(bound))
    {
        HqlExprArray args;
        callProcedure(ctx, DecPopLongAtom, args);
        bound.expr.set(NULL);
    }
}

void HqlCppTranslator::expandTranslated(IHqlExpression * expr, CHqlBoundExpr & tgt)
{
    tgt.setFromTranslated(expr);
}

void HqlCppTranslator::buildCachedExpr(BuildCtx & ctx, IHqlExpression * expr, CHqlBoundExpr & tgt)
{
    buildExpr(ctx, expr, tgt);
}

void HqlCppTranslator::buildAnyExpr(BuildCtx & ctx, IHqlExpression * expr, CHqlBoundExpr & tgt)
{
    if (expr->isDataset())
        buildDataset(ctx, expr, tgt, FormatNatural);
    else if (expr->isDatarow())
    {
        Owned<IReferenceSelector> selector = buildNewRow(ctx, expr);
        selector->buildAddress(ctx, tgt);
    }
    else
        buildExpr(ctx, expr, tgt);
}

void HqlCppTranslator::buildExpr(BuildCtx & ctx, IHqlExpression * expr, CHqlBoundExpr & tgt)
{
    node_operator op = expr->getOperator();

    switch (op)
    {
    case no_counter:
        doBuildExprCounter(ctx, expr, tgt);
        return;
    case no_countfile:
        doBuildExprCountFile(ctx, expr, tgt);
        return;
    case no_countindex:
        doBuildExprCountIndex(ctx, expr, tgt);
        return;
    case no_evaluate:
        doBuildExprEvaluate(ctx, expr, tgt);
        return;
    case no_thor:
        throwUnexpected();
//      assertex(expr->queryType()->isScalar());
//      buildExpr(ctx, expr->queryChild(0), tgt);
        return;
    case no_count:
        if (!(expr->isPure() && ctx.getMatchExpr(expr, tgt)))
            doBuildExprCount(ctx, expr, tgt);
        return;
    case no_max:
    case no_min:
    case no_sum:
        if (!(expr->isPure() && ctx.getMatchExpr(expr, tgt)))
            doBuildExprAggregate(ctx, expr, tgt);
        return;
    case no_exists:
    case no_notexists:
        if (!(expr->isPure() && ctx.getMatchExpr(expr, tgt)))
            doBuildExprExists(ctx, expr, tgt);
        return;
    case no_existslist:
        doBuildAggregateList(ctx, NULL, expr, &tgt);
        return;
    case no_countlist:
        doBuildAggregateList(ctx, NULL, expr, &tgt);
        return;
    case no_sumlist:
        doBuildAggregateList(ctx, NULL, expr, &tgt);
        return;
    case no_minlist:
        doBuildAggregateList(ctx, NULL, expr, &tgt);
        return;
    case no_maxlist:
        doBuildAggregateList(ctx, NULL, expr, &tgt);
        return;
    case no_sizeof:
        doBuildExprSizeof(ctx, expr, tgt);
        return;
    case no_filepos:
        doBuildExprFilepos(ctx, expr, tgt);
        return;
    case no_file_logicalname:
        doBuildExprFileLogicalName(ctx, expr, tgt);
        return;
    case no_getresult:
    case no_workunit_dataset:
        doBuildExprGetResult(ctx, expr, tgt);
        return;
    case no_getgraphresult:
        doBuildExprGetGraphResult(ctx, expr, tgt, FormatNatural);
        return;
    case no_regex_find:
    case no_regex_replace:
        doBuildExprRegexFindReplace(ctx, expr, tgt);
        return;
    case no_skip:
    case no_assert:
        {
            buildStmt(ctx, expr);
            OwnedHqlExpr null = createNullExpr(expr);
            buildExpr(ctx, null, tgt); 
            return;
        }
    case no_matched:
    case no_matchtext:
    case no_matchlength:
    case no_matchposition:
    case no_matchunicode:
    case no_matchutf8:
        doBuildMatched(ctx, NULL, expr, &tgt);
        return;
    case no_matchattr:
        doBuildMatchAttr(ctx, NULL, expr, &tgt);
        return;
    case no_rowdiff:
        doBuildExprRowDiff(ctx, expr, tgt);
        return;
    case no_xmltext:
        doBuildExprXmlText(ctx, expr, tgt);
        return;
    case no_xmlunicode:
        doBuildExprXmlUnicode(ctx, expr, tgt);
        return;
    case no_evalonce:
        doBuildEvalOnce(ctx, NULL, expr, &tgt);
        return;
    case no_alias:
        doBuildExprAlias(ctx, expr, &tgt);
        return;
    case no_alias_scope:
        {
            expandAliasScope(ctx, expr);
            buildExpr(ctx, expr->queryChild(0), tgt);
            return;
        }
    case no_between:
    case no_notbetween:
        {
            OwnedHqlExpr between = expandBetween(expr);
            buildExpr(ctx, between, tgt);
            return;
        }
    case no_libraryinput:
        doBuildAliasValue(ctx, expr, tgt);
        return;
    case no_externalcall:
    case no_call:
        doBuildExprCall(ctx, expr, tgt);
        return;
    case no_comma:
    case no_compound:
        buildStmt(ctx, expr->queryChild(0));
        buildExpr(ctx, expr->queryChild(1), tgt);
        return;
    case no_cast:
    case no_implicitcast:
        doBuildExprCast(ctx, expr, tgt);
        return;
    case no_charlen:
        tgt.expr.setown(doBuildCharLength(ctx, expr->queryChild(0)));
        return;
    case no_add:
        doBuildExprAdd(ctx, expr, tgt);
        return;
    case no_mul:
    case no_sub:
        doBuildExprArith(ctx, expr, tgt);
        return;
    case no_abs:
        doBuildExprAbs(ctx, expr, tgt);
        return;
    case no_negate:
        doBuildExprNegate(ctx, expr, tgt);
        return;
    case no_div:
    case no_modulus:
        doBuildExprDivide(ctx, expr, tgt);
        return;
    case no_if:
        doBuildExprIf(ctx, expr, tgt);
        return;
    case no_index:
        doBuildExprIndex(ctx, expr, tgt);
        return;
    case no_list:
        doBuildExprList(ctx, expr, tgt);
        return;
    case no_all:
        doBuildExprAll(ctx, expr, tgt);
        return;
    case no_trim:
        doBuildExprTrim(ctx, expr, tgt);
        return;
    case no_intformat:
        doBuildExprFormat(intFormatAtom, ctx, expr, tgt);
        return;
    case no_realformat:
        doBuildExprFormat(realFormatAtom, ctx, expr, tgt);
        return;
    case no_exp:
        doBuildExprSysFunc(ctx, expr, tgt, clibExpIdAtom);
        return;
    case no_ln:
        doBuildExprSysFunc(ctx, expr, tgt, lnAtom);
        return;
    case no_sin:
        doBuildExprSysFunc(ctx, expr, tgt, sinAtom);
        return;
    case no_cos:
        doBuildExprSysFunc(ctx, expr, tgt, cosAtom);
        return;
    case no_tan:
        doBuildExprSysFunc(ctx, expr, tgt, tanAtom);
        return;
    case no_asin:
        doBuildExprSysFunc(ctx, expr, tgt, asinAtom);
        return;
    case no_acos:
        doBuildExprSysFunc(ctx, expr, tgt, acosAtom);
        return;
    case no_atan:
        doBuildExprSysFunc(ctx, expr, tgt, atanAtom);
        return;
    case no_atan2:
        doBuildExprSysFunc(ctx, expr, tgt, atan2Atom);
        return;
    case no_sinh:
        doBuildExprSysFunc(ctx, expr, tgt, sinhAtom);
        return;
    case no_cosh:
        doBuildExprSysFunc(ctx, expr, tgt, coshAtom);
        return;
    case no_tanh:
        doBuildExprSysFunc(ctx, expr, tgt, tanhAtom);
        return;
    case no_log10:
        doBuildExprSysFunc(ctx, expr, tgt, log10Atom);
        return;
    case no_power:
        doBuildExprSysFunc(ctx, expr, tgt, powerAtom);
        return;
    case no_fail:
        doBuildStmtFail(ctx, expr);
        tgt.expr.setown(createConstant(0));
        return;
    case no_failcode:
        doBuildExprFailCode(ctx, expr, tgt);
        return;
    case no_ordered:
        doBuildExprOrdered(ctx, expr, tgt);
        return;
    case no_random:
        doBuildExprSysFunc(ctx, expr, tgt, rtlRandomAtom);
        return;
    case no_recovering:
        doBuildExprSysFunc(ctx, expr, tgt, getRecoveringCountAtom);
        return;
    case no_rank:
        doBuildExprRank(ctx, expr, tgt);
        return;
    case no_ranked:
        doBuildExprRanked(ctx, expr, tgt);
        return;
    case no_round:
    case no_roundup:
        doBuildExprRound(ctx, expr, tgt);
        return;
    case no_sqrt:
        doBuildExprSysFunc(ctx, expr, tgt, sqrtAtom);
        return;
    case no_truncate:
        doBuildExprSysFunc(ctx, expr, tgt, truncateAtom);
        return;
    case no_offsetof:
        doBuildExprOffsetOf(ctx, expr, tgt);
        return;
    case no_substring:
        doBuildExprSubString(ctx, expr, tgt);
        return;
    case no_in:
    case no_notin:
        {
            if (expr->queryChild(1)->getOperator() == no_all)
                tgt.expr.setown(createConstant(op == no_in));
            else
            {
                OwnedHqlExpr optimized = querySimplifyInExpr(expr);
                if (options.globalFold && optimized)
                {
                    OwnedHqlExpr folded = foldHqlExpression(optimized);
                    buildExpr(ctx, folded, tgt);
                }
                else
                    buildTempExpr(ctx, expr, tgt);
            }
            return;
        }
    case no_case:
    case no_choose:
    case no_concat:
    case no_crc:
    case no_hash:
    case no_hash32:
    case no_hash64:
    case no_hashmd5:
    case no_map:
    case no_order:
    case no_unicodeorder:
    case no_rejected:
    case no_which:
    case no_addsets:
    case no_createset:
    case no_catch:
    case no_failmessage:
    case no_eventname:
    case no_eventextra:
    case no_loopcounter:
    case no_toxml:
        buildTempExpr(ctx, expr, tgt);
        return;
    case no_asstring:
    case no_typetransfer:
        doBuildExprTransfer(ctx, expr, tgt);
        return;
    case no_translated:
        {
            expandTranslated(expr, tgt);
            return;
        }
    case no_eq:
    case no_ne:
    case no_le:
    case no_lt:
    case no_ge:
    case no_gt:
        if (options.expressionPeephole)
        {
            OwnedHqlExpr optimized = peepholeOptimize(ctx, expr);
            if (optimized != expr)
            {
                buildExpr(ctx, optimized, tgt);
                return;
            }
        }
        doBuildExprCompare(ctx, expr, tgt);
        return;
    case no_wuid:
        doBuildExprWuid(ctx, expr, tgt);
        return;
    case no_getenv:
        {
            OwnedHqlExpr mapped = cvtGetEnvToCall(expr);
            buildExpr(ctx, mapped, tgt);
            return;
        }
    case no_notnot:
        {
            OwnedHqlExpr castChild = ensureExprType(expr->queryChild(0), queryBoolType());
            buildExpr(ctx, castChild, tgt);
        }
        return;
    case no_not:
        {
            IHqlExpression * child = expr->queryChild(0);
            node_operator childOp = child->getOperator();
            if (((childOp == no_and) || (childOp == no_or)) && requiresTempAfterFirst(ctx, child))
                buildTempExpr(ctx, expr, tgt);
            else
                doBuildExprNot(ctx, expr, tgt);
            return;
        }
    case no_constant:
        {
            ITypeInfo * type = expr->queryType();
            if ((options.inlineStringThreshold > 0) && (type->getSize() > options.inlineStringThreshold) && (type->getSize() != UNKNOWN_LENGTH))
            {
                IHqlExpression * literal = addBigLiteral((const char *)expr->queryValue()->queryValue(), type->getSize());
                Owned<ITypeInfo> retType = makeReferenceModifier(LINK(type));
                switch (type->getTypeCode())
                {
                case type_unicode:
                case type_varunicode:
                case type_utf8:
                    literal = createValue(no_implicitcast, LINK(retType), literal);
                    break;
                }
                if (literal->queryType() != retType)
                    literal = createValue(no_typetransfer, LINK(retType), literal);
                tgt.expr.setown(literal);
            }
            else
                tgt.expr.set(expr);
            return;
        }
    case no_quoted:
    case no_variable:
        tgt.expr.set(expr);
        return;
    case no_globalscope:
        if (options.regressionTest)
            throwUnexpected();
        buildExpr(ctx, expr->queryChild(0), tgt);
        return;
    case no_nofold:
    case no_nohoist:
    case no_section:
    case no_sectioninput:
    case no_pure:
        buildExpr(ctx, expr->queryChild(0), tgt);
        return;
    case no_band:
    case no_bor:
    case no_bnot:
    case no_bxor:
    case no_lshift:
    case no_rshift:
        doBuildPureSubExpr(ctx, expr, tgt);
        return;
        //MORE: Shouldn't these be special cased?
    case no_xor:
        doBuildPureSubExpr(ctx, expr, tgt);
        return;
    case no_select:
        {
            OwnedHqlExpr aggregate = convertToSimpleAggregate(expr);
            if (aggregate && canProcessInline(&ctx, aggregate->queryChild(0)))
            {
                buildExpr(ctx, aggregate, tgt);
                return;
            }
            Owned<IReferenceSelector> selector = buildReference(ctx, expr);
            selector->get(ctx, tgt);
            return;
        }
    case no_field:
        throwUnexpected();
    case no_is_null:
        {
            //Until we have something better in place isNull is the inverse of isValid().
            IHqlExpression * child = expr->queryChild(0);
            OwnedHqlExpr null = createValue(no_not, makeBoolType(), createValue(no_is_valid, makeBoolType(), LINK(child)));
            buildExpr(ctx, null, tgt);
        }
        return;
    case no_is_valid:
        doBuildExprIsValid(ctx, expr, tgt);
        return;
    case no_fromunicode:
    case no_tounicode:
        doBuildExprToFromUnicode(ctx, expr, tgt);
        return;
    case no_keyunicode:
        doBuildExprKeyUnicode(ctx, expr, tgt);
        return;
    case no_xmldecode:
    case no_xmlencode:
        buildTempExpr(ctx, expr, tgt);
        return;
    case no_and:
    case no_or:
        if (requiresTempAfterFirst(ctx, expr))
            buildTempExpr(ctx, expr, tgt);
        else
            doBuildPureSubExpr(ctx, expr, tgt);
        return;
    case no_assertkeyed:
    case no_assertwild:
        {
            StringBuffer s;
            throwError1(HQLERR_KeyedWildNoIndex, getExprECL(expr, s).str());
        }
    case no_assertstepped:
        {
            StringBuffer s;
            throwError1(HQLERR_SteppedNoJoin, getExprECL(expr, s).str());
        }
    case no_id2blob:
        doBuildExprIdToBlob(ctx, expr, tgt);
        return;
    case no_blob2id:
        doBuildExprBlobToId(ctx, expr, tgt);
        return;
    case no_cppbody:
        doBuildExprCppBody(ctx, expr, &tgt);
        return;
    case no_null:
        tgt.length.setown(getSizetConstant(0));
        tgt.expr.setown(createValue(no_nullptr, makeReferenceModifier(expr->getType())));
        return;
    case no_clustersize:
        doBuildExprSysFunc(ctx, expr, tgt, getClusterSizeAtom);
        return;
    case no_deref:
        //Untested
        buildExpr(ctx, expr->queryChild(0), tgt);
        if (tgt.expr->getOperator() == no_address)
            tgt.expr.setown(createValue(no_typetransfer, expr->getType(), LINK(tgt.expr->queryChild(0))));
        else
            tgt.expr.setown(createValue(no_deref, expr->getType(), LINK(tgt.expr)));
        return;
    case no_funcdef:
        tgt.expr.setown(doBuildInternalFunction(expr));
        useFunction(tgt.expr);
        return;
    case no_purevirtual:
    case no_internalvirtual:
        {
            //This shouldn't happen we should have an no_checkconcrete wrapper inserted into the tree like checkconstant,
            //but it currently can in obscure library contexts (e.g., library3ie2.xhql)
            _ATOM name = expr->queryName();
            throwError1(HQLERR_ConcreteMemberRequired, name ? name->str() : "");
        }
    case NO_AGGREGATEGROUP:
        throwError1(HQLERR_OutsideGroupAggregate, getOpString(op));
    default:
        break;
    }

    StringBuffer msg;
    msg.append("Unexpected operator '").append(getOpString(op)).append("' in: HqlCppTranslator::buildExpr(");
    toECL(expr, msg, true);
//  expr->toString(msg);
    msg.append(")");
    throw MakeStringException(HQLERR_UnexpectedOperator, msg.str());
    doBuildPureSubExpr(ctx, expr, tgt);
}


void HqlCppTranslator::buildExprOrAssign(BuildCtx & ctx, const CHqlBoundTarget * target, IHqlExpression * expr, CHqlBoundExpr * tgt)
{
    if (target)
        buildExprAssign(ctx, *target, expr);
    else if (tgt)
        buildExpr(ctx, expr, *tgt);
    else
        buildStmt(ctx, expr);
}


bool HqlCppTranslator::specialCaseBoolReturn(BuildCtx & ctx, IHqlExpression * expr)
{
    if (!options.optimizeBoolReturn)
        return false;
    if ((expr->getOperator() == no_and) && (unwoundCount(expr, no_and) <= MAX_NESTED_CASES))
        return true;
    if (expr->getOperator() == no_not)
        expr = expr->queryChild(0);
    if (!requiresTemp(ctx, expr, true))
        return false;
    if (expr->getOperator() == no_alias_scope)
        expr = expr->queryChild(0);
    if ((expr->getOperator() == no_and) || (expr->getOperator() == no_or))
        return true;
    return false;
}

void HqlCppTranslator::buildReturn(BuildCtx & ctx, IHqlExpression * expr, ITypeInfo * retType)
{
    ITypeInfo * exprType = expr->queryType();
    if (!retType) retType = exprType;

    expr = queryExpandAliasScope(ctx, expr);

    node_operator op = expr->getOperator();
    if ((retType->getSize() == UNKNOWN_LENGTH) && (retType->getTypeCode() == type_varstring))
    {
        if (hasConstModifier(retType) && (hasConstModifier(exprType) || expr->queryValue()))
        {
            OwnedHqlExpr cast = ensureExprType(expr, retType);
            CHqlBoundExpr ret;
            buildCachedExpr(ctx, cast, ret);
            ctx.setNextDestructor();
            ctx.addReturn(ret.expr);
        }
        else
        {
            if (hasConstModifier(retType))
            {
                BuildCtx * declareCtx = NULL;
                if (getInvariantMemberContext(ctx, &declareCtx, NULL, false, false))
                {
                    CHqlBoundTarget tempTarget;
                    createTempFor(*declareCtx, retType, tempTarget, typemod_member, FormatNatural);
                    buildExprAssign(ctx, tempTarget, expr);
                    OwnedHqlExpr result = getElementPointer(tempTarget.expr);
                    ctx.addReturn(result);
                    return;
                }

                //we are going to have a memory leak......
                PrintLog("Runtime memory leak returning allocated string from constant function");
            }

            CHqlBoundTarget retVar;
            retVar.expr.setown(createWrapperTemp(ctx, retType, typemod_none));

            buildExprAssign(ctx, retVar, expr);
            ctx.setNextDestructor();

            StringBuffer s;
            retVar.expr->toString(s);
            switch (retType->getTypeCode())
            {
            case type_varstring:
                s.append(".detachstr()");
                break;
            case type_varunicode:
                s.append(".detachustr()");
                break;
            default:
                UNIMPLEMENTED;
            }

            OwnedHqlExpr temp = createQuoted(s.str(), LINK(exprType));
            ctx.addReturn(temp);
        }
    }
    else if ((retType->getTypeCode() == type_boolean) && specialCaseBoolReturn(ctx, expr))
    {
        bool successValue = true;
        if (op == no_not)
        {
            //!(a and b) is converted into !a || !b.  Otherwise just invert the test condition.
            IHqlExpression * child = expr->queryChild(0);
            if (child->getOperator() == no_alias_scope)
                child = child->queryChild(0);
            if (child->getOperator() != no_and)
            {
                successValue = false;
                expr = expr->queryChild(0);
            }
        }

        BuildCtx condctx(ctx);
        buildFilteredReturn(condctx, expr, queryBoolExpr(successValue));
        buildReturn(ctx, queryBoolExpr(!successValue));
    }
    else if (op == no_if)
    {
        OwnedHqlExpr castTrue = ensureExprType(expr->queryChild(1), retType);
        OwnedHqlExpr castFalse = ensureExprType(expr->queryChild(2), retType);

        BuildCtx condctx(ctx);
        buildFilter(condctx, expr->queryChild(0));
        buildReturn(condctx, castTrue);
        buildReturn(ctx, castFalse);
    }
    else if (op == no_map || op == no_case)
    {
        HqlCppCaseInfo info(*this);
        doBuildCaseInfo(expr, info);
        info.buildReturn(ctx);
    }
    else
    {
        CHqlBoundExpr ret;
        // A very temporary work around for potential gpf using variable length strings
        OwnedHqlExpr castExpr = ensureExprType(expr, retType);
        buildSimpleExpr(ctx, castExpr, ret);
        ctx.setNextDestructor();
        ctx.addReturn(ret.expr);
    }
}

//Assumes that the value being returned is simple.
//for (a || b || c) gen if (a) return x; if (b) return x; if (c) return x;
//and !(a && b && c) -> !a || !b || !c
void HqlCppTranslator::buildFilteredReturn(BuildCtx & ctx, IHqlExpression * filter, IHqlExpression * value)
{
    filter = queryExpandAliasScope(ctx, filter);

    HqlExprArray conds;
    node_operator op = filter->getOperator();
    if (op == no_or)
    {
        buildFilteredReturn(ctx, filter->queryChild(0), value);
        buildFilteredReturn(ctx, filter->queryChild(1), value);
        return;
    }
    if (op == no_not)
    {
        IHqlExpression * child = filter->queryChild(0);
        node_operator childOp = child->getOperator();
        if (childOp == no_and)
        {
            child->unwindList(conds, no_and);
            ForEachItemIn(i, conds)
            {
                IHqlExpression & cur = conds.item(i);
                OwnedHqlExpr inverse = getInverse(&cur);
                buildFilteredReturn(ctx, inverse, value);
            }
            return;
        }
        if (childOp == no_alias_scope)
        {
            expandAliasScope(ctx, child);
            OwnedHqlExpr inverse = getInverse(child->queryChild(0));
            buildFilteredReturn(ctx, inverse, value);
            return;
        }
    }

    BuildCtx condctx(ctx);
    buildFilter(condctx, filter);
    if (value)
        buildReturn(condctx, value);
    else
        condctx.addReturn(NULL);
}

void HqlCppTranslator::buildStmt(BuildCtx & _ctx, IHqlExpression * expr)
{
    BuildCtx ctx(_ctx);

    switch (expr->getOperator())
    {
    case no_assign:
        doBuildStmtAssign(ctx, expr->queryChild(0), expr->queryChild(1));
        return;
    case no_assign_addfiles:
        doBuildStmtAssignModify(ctx, expr->queryChild(0), expr->queryChild(1), expr->getOperator());
        return;
    case no_alias:
        doBuildExprAlias(ctx, expr, NULL);
        return;
    case no_alias_scope:
        {
            expandAliasScope(ctx, expr);
            buildStmt(ctx, expr->queryChild(0));
            return;
        }
    case no_assignall:
        {
            unsigned idx;
            unsigned kids = expr->numChildren();
            for (idx = 0; idx < kids; idx++)
                buildStmt(ctx, expr->queryChild(idx));
            return;
        }
    case no_comma:
    case no_compound:
        buildStmt(ctx, expr->queryChild(0));
        buildStmt(ctx, expr->queryChild(1));
        return;
    case no_if:
        doBuildStmtIf(ctx, expr);
        return;
    case no_call:
    case no_externalcall:
        doBuildStmtCall(ctx, expr);
        return;
    case no_nofold:
    case no_nohoist:
    case no_nothor:
    case no_section:
    case no_sectioninput:
        buildStmt(ctx, expr->queryChild(0));
        return;
    case no_null:
        return;
    case no_fail:
        doBuildStmtFail(ctx, expr);
        return;
    case no_setmeta:
        return;
    case no_update:
        doBuildStmtUpdate(ctx, expr);
        return;
    case no_output:
        if (queryRealChild(expr, 1))
            throwError1(HQLERR_NotSupportedInsideNoThor, "OUTPUT to file");
        doBuildStmtOutput(ctx, expr);
        return;
    case no_subgraph:
        doBuildThorChildSubGraph(ctx, expr, SubGraphRoot);
        return;
    case no_thor:
        doBuildThorGraph(ctx, expr);
        return;
    case no_workflow_action:
        return;
    case no_ensureresult:
        doBuildStmtEnsureResult(ctx, expr);
        return;
    case no_extractresult:
    case no_setresult:
        doBuildStmtSetResult(ctx, expr);
        return;
    case no_parallel:
    case no_sequential:
    case no_actionlist:
        {
            ForEachChild(idx, expr)
                buildStmt(ctx, expr->queryChild(idx));
            return;
        }
    case no_wait:
        doBuildStmtWait(ctx, expr);
        return;
    case no_notify:
        doBuildStmtNotify(ctx, expr);
        return;
    case no_skip:
        doBuildStmtSkip(ctx, expr, NULL);
        return;
    case no_assert:
        doBuildStmtAssert(ctx, expr);
        return;
    case no_cppbody:
        doBuildExprCppBody(ctx, expr, NULL);
        return;
    case no_setworkflow_cond:
        {
            HqlExprArray args;
            args.append(*LINK(expr->queryChild(0)));
            buildFunctionCall(ctx, setWorkflowConditionAtom, args);
            return;
        }
    case no_apply:
        doBuildStmtApply(ctx, expr);
        return;
    case no_cluster:
        doBuildStmtCluster(ctx, expr);
        return;
    case no_choose:
        doBuildChoose(ctx, NULL, expr);
        return;
    case no_persist_check:
        buildWorkflowPersistCheck(ctx, expr);
        return;
    case no_evaluate_stmt:
        expr = expr->queryChild(0);
        if (expr->queryValue())
            return;
        // fall through to default behaviour.
    }
    CHqlBoundExpr tgt;
    buildAnyExpr(ctx, expr, tgt);
    ctx.addExpr(tgt.expr);
    tidyupExpr(ctx, tgt);
}


class AliasExpansionInfo
{
public:
    void pushCondition(IHqlExpression * expr, unsigned branch)  { conditions.append(*createAttribute(branchAtom, LINK(expr), getSizetConstant(branch))); }
    void popCondition() { conditions.pop(); }
    void popConditions(unsigned num) { conditions.popn(num); }

    bool isConditional() { return conditions.ordinality() != 0; }

    IHqlExpression * createConditionIntersection(IHqlExpression * prev)
    {
        if (conditions.ordinality() == 0)
            return NULL;

        if (!prev)
            return createValueSafe(no_sortlist, makeSortListType(NULL), conditions);

        unsigned maxPrev = prev->numChildren();
        unsigned max = maxPrev;
        if (max > conditions.ordinality())
            max = conditions.ordinality();

        for (unsigned i=0; i < max; i++)
        {
            if (&conditions.item(i) != prev->queryChild(i))
            {
                if (i == 0)
                    return NULL;
                return createValueSafe(no_sortlist, makeSortListType(NULL), conditions, 0, i);
            }
        }
        if (max == maxPrev)
            return LINK(prev);
        return createValueSafe(no_sortlist, makeSortListType(NULL), conditions);
    }

    HqlExprArray conditions;
};


void HqlCppTranslator::doExpandAliases(BuildCtx & ctx, IHqlExpression * expr, AliasExpansionInfo & info)
{
    IHqlExpression * prev = static_cast<IHqlExpression *>(expr->queryTransformExtra());
    if (prev == expr)
        return;

#ifdef USE_NEW_ALIAS_CODE
    OwnedHqlExpr commonPath = info.createConditionIntersection(prev);
    if (prev == commonPath)
        return;

    if (commonPath)
        expr->setTransformExtra(commonPath);
    else
        expr->setTransformExtraUnlinked(expr);

    node_operator op = expr->getOperator();
    switch (op)
    {
        //MORE: Anything that creates a child query shouldn't be included here...
    case no_select:
    case NO_AGGREGATE:
    case no_alias_scope:
        break;
    case no_alias:
        {
            IHqlExpression * value = expr->queryChild(0);
            if ((commonPath == NULL) && !ctx.queryMatchExpr(value))
            {
                if (containsAliasLocally(value) && !expr->hasProperty(globalAtom))
                    doExpandAliases(ctx, value, info);
                doBuildExprAlias(ctx, expr, NULL);
            }
            break;
        }
    case no_and:
    case no_or:
        {
            HqlExprArray args;
            expr->unwindList(args, op);

            doExpandAliases(ctx, &args.item(0), info);
            unsigned max = args.ordinality();
            for (unsigned i=1; i < max; i++)
            {
                info.pushCondition(expr, i);
                doExpandAliases(ctx, &args.item(i), info);
            }

            info.popConditions(max-1);
            break;
        }
    case no_if:
        {
            doExpandAliases(ctx, expr->queryChild(0), info);
            info.pushCondition(expr, 1);
            doExpandAliases(ctx, expr->queryChild(1), info);
            info.popCondition();
            info.pushCondition(expr, 2);
            doExpandAliases(ctx, expr->queryChild(2), info);
            info.popCondition();
            break;
        }
    case no_case:
        {
            doExpandAliases(ctx, expr->queryChild(0), info);
            unsigned max = expr->numChildren();
            for (unsigned i=1; i < max-1; i++)
            {
                info.pushCondition(expr, i*2);
                doExpandAliases(ctx, expr->queryChild(i)->queryChild(0), info);
                info.popCondition();
                info.pushCondition(expr, i*2+1);
                doExpandAliases(ctx, expr->queryChild(i)->queryChild(1), info);
                info.popCondition();
            }
            info.pushCondition(expr, (max-1)*2);
            doExpandAliases(ctx, expr->queryChild(max-1), info);
            info.popCondition();
            break;
        }
    case no_map:
        {
            //The following is equivalent to old, code; I'm not sure it is the best implementation
            unsigned max = expr->numChildren();
            for (unsigned i=0; i < max-1; i++)
            {
                info.pushCondition(expr, i*2);
                doExpandAliases(ctx, expr->queryChild(i)->queryChild(0), info);
                info.popCondition();
                info.pushCondition(expr, i*2+1);
                doExpandAliases(ctx, expr->queryChild(i)->queryChild(1), info);
                info.popCondition();
            }
            info.pushCondition(expr, (max-1)*2);
            doExpandAliases(ctx, expr->queryChild(max-1), info);
            info.popCondition();
            break;
        }
    default:
        if (containsAliasLocally(expr))
        {
            ForEachChild(i, expr)
                doExpandAliases(ctx, expr->queryChild(i), info);
        }
        break;
    }
#else
    expr->setTransformExtraUnlinked(expr);

    node_operator op = expr->getOperator();
    switch (op)
    {
        //MORE: Anything that creates a child query shouldn't be included here...
    case no_select:
    case NO_AGGREGATE:
    case no_alias_scope:
        break;
    case no_alias:
        {
            doBuildExprAlias(ctx, expr, NULL);
            break;
        }
    default:
        if (containsAliasLocally(expr))
        {
            ForEachChild(i, expr)
                doExpandAliases(ctx, expr->queryChild(i), info);
        }
        break;
    }
#endif
}

void HqlCppTranslator::expandAliases(BuildCtx & ctx, IHqlExpression * expr)
{
    if (containsAliasLocally(expr))
    {
        TransformMutexBlock block;
        AliasExpansionInfo info;
        doExpandAliases(ctx, expr, info);
    }
}


void HqlCppTranslator::expandAliasScope(BuildCtx & ctx, IHqlExpression * expr)
{
    TransformMutexBlock block;
    AliasExpansionInfo info;
    unsigned max = expr->numChildren();
    for (unsigned idx = 1; idx < max; idx++)
    {
        IHqlExpression * child = expr->queryChild(idx);
        if (containsAliasLocally(child))
            doExpandAliases(ctx, child, info);
    }
}

//------------------------------------------------------------------------------

void HqlCppTranslator::gatherActiveCursors(BuildCtx & ctx, HqlExprCopyArray & activeRows)
{
    AssociationIterator iter(ctx);
    ForEach(iter)
    {
        HqlExprAssociation & cur = iter.get();
        if (cur.isRowAssociation())
        {
            BoundRow & curRow = static_cast<BoundRow &>(cur);
            if ((curRow.querySide() != no_self) && !curRow.isBuilder())
                activeRows.append(*curRow.querySelector());
        }
        else if (cur.represents->getOperator() == no_counter)
            activeRows.append(*cur.represents);
    }
}

bool HqlCppTranslator::canEvaluateInContext(BuildCtx & ctx, IHqlExpression * expr)
{
    HqlExprCopyArray cursors;
    gatherActiveCursors(ctx, cursors);
    return ::canEvaluateInScope(cursors, expr);
}



bool mustEvaluateInContext(BuildCtx & ctx, IHqlExpression * expr)
{
    HqlExprCopyArray required;
    expr->gatherTablesUsed(NULL, &required);
    if (required.ordinality() == 0)
        return false;

    HqlExprCopyArray activeRows;
    HqlExprCopyArray inheritedRows;
    RowAssociationIterator iter(ctx);
    ForEach(iter)
    {
        BoundRow & cur = iter.get();
        if ((cur.querySide() != no_self) && !cur.isBuilder())
        {
            IHqlExpression * selector = cur.querySelector();
            if (cur.isInherited())
                inheritedRows.append(*selector);
            else
                activeRows.append(*selector);
        }
    }

    //Ensure all instances of activeRows which match the inherited rows are removed
    ForEachItemInRev(i, activeRows)
    {
        if (inheritedRows.find(activeRows.item(i)))
            activeRows.remove(i);
    }

    return canEvaluateInScope(activeRows, required);
}


bool filterIsTableInvariant(IHqlExpression * expr)
{
    IHqlExpression * dsSelector = expr->queryChild(0)->queryNormalizedSelector();
    ForEachChildFrom(i, expr, 1)
    {
        IHqlExpression * cur = expr->queryChild(i);
        if (containsSelector(cur, dsSelector))
            return false;
    }
    return true;
}

//-----------------------------------------------------------------------------

bool LoopInvariantHelper::getBestContext(BuildCtx & ctx, IHqlExpression * expr)
{
    finished();

    active = ctx.selectBestContext(expr);
    return (active != NULL);
}

void LoopInvariantHelper::finished()
{
    if (active)
    {
        active->mergeScopeWithContainer();
        active = NULL;
    }
}


//---------------------------------------------------------------------------

void HqlCppTranslator::buildBlockCopy(BuildCtx & ctx, IHqlExpression * tgt, CHqlBoundExpr & src)
{
    OwnedHqlExpr size = getBoundSize(src);
    if (!size->queryValue() || size->queryValue()->getIntValue() != 0)
    {
        HqlExprArray args;
        args.append(*getPointer(tgt));
        args.append(*getPointer(src.expr));
        args.append(*size.getClear());
        callProcedure(ctx, memcpyAtom, args);
    }
}

void HqlCppTranslator::buildSimpleExpr(BuildCtx & ctx, IHqlExpression * expr, CHqlBoundExpr & tgt)
{
    node_operator op = expr->getOperator();

    bool simple = false;
    switch (op)
    {
    case no_alias_scope:
        {
            expandAliasScope(ctx, expr);
            buildSimpleExpr(ctx, expr->queryChild(0), tgt);
            return;
        }
    case no_preservemeta:
        buildSimpleExpr(ctx, expr->queryChild(0), tgt);
        return;
    case no_constant:
    case no_variable:
    case no_getresult:      // gets forced into a variable.
    case no_workunit_dataset:   // gets forced into a variable.
    case no_getgraphresult:     // gets forced into a variable.
    case no_alias:
    case no_list:
    case no_all:
    case no_null:
    case no_id2blob:
    case no_rows:
    case no_libraryinput:
        simple = true;
        break;
    case no_compound:
        buildStmt(ctx, expr->queryChild(0));
        buildSimpleExpr(ctx, expr->queryChild(1), tgt);
        return;
    case no_field:
        throwUnexpected();
    case no_select:
        {
            CHqlBoundExpr bound;
            buildCachedExpr(ctx, expr, bound);
            if (isSimpleTranslatedExpr(bound.expr))
                tgt.set(bound);
            else
            {
                OwnedHqlExpr trans = bound.getTranslatedExpr();
                buildTempExpr(ctx, trans, tgt);
            }
            return;
        }
        break;      // should depend on whether conditional etc....
    case no_translated:
        simple =  isSimpleTranslatedExpr(expr->queryChild(0));
        break;
    case no_substring:
        {
            SubStringInfo info(expr);
            if (info.canGenerateInline() || expr->hasProperty(quickAtom))
                simple = true;
            break;
        }
    case no_cast:
    case no_implicitcast:
        {
            //special case casting a string to (string) - saves lots of temporaries.
            ITypeInfo * exprType = expr->queryType();
            IHqlExpression * child = expr->queryChild(0);
            ITypeInfo * childType = child->queryType();
            if ((exprType->getTypeCode() == type_string) && (exprType->getSize() == UNKNOWN_LENGTH))
            {
                if ((childType->getTypeCode() == type_string) && (exprType->queryCharset() == childType->queryCharset()))
                {
                    buildSimpleExpr(ctx, child, tgt);
                    return;
                }
            }
            if (options.foldConstantCast && (child->getOperator() == no_constant))
                simple = true;
            break;
        }
    case no_sizeof:
    case no_offsetof:
        simple = true;
        break;
    case no_regex_find:
        simple = expr->isBoolean();
        break;
    case no_call:
    case no_externalcall:
        {
            ITypeInfo * type = expr->queryType();
            switch (type->getTypeCode())
            {
            case type_set:
                simple = true;
                break;
            case type_string:
            case type_data:
            case type_qstring:
                if (type->getSize() == UNKNOWN_LENGTH)
                    simple = true;
                break;
            }
        }
    }

    if (simple)
        buildCachedExpr(ctx, expr, tgt);
    else
        buildTempExpr(ctx, expr, tgt);
}


IHqlExpression * HqlCppTranslator::buildSimplifyExpr(BuildCtx & ctx, IHqlExpression * expr)
{
    node_operator op = expr->getOperator();

    switch (op)
    {
    case no_constant:
    case no_all:
    case no_null:
        return LINK(expr);
    case no_list:
        if (expr->isConstant())
            return LINK(expr);
        break;
    }

    CHqlBoundExpr bound;
    buildSimpleExpr(ctx, expr, bound);
    return bound.getTranslatedExpr();
}

/* In type: not linked. Return: linked */
IHqlExpression * HqlCppTranslator::createWrapperTemp(BuildCtx & ctx, ITypeInfo * type, typemod_t modifier)
{
    Linked<ITypeInfo> rawType = queryUnqualifiedType(type);
    if (hasLinkCountedModifier(type))
        rawType.setown(makeAttributeModifier(rawType.getClear(), getLinkCountedAttr()));

    Owned<ITypeInfo> declType = makeWrapperModifier(rawType.getClear());
    declType.setown(makeModifier(declType.getClear(), modifier));

    return ctx.getTempDeclare(declType, NULL);
}

void HqlCppTranslator::createTempFor(BuildCtx & ctx, IHqlExpression * expr, CHqlBoundTarget & target)
{
    createTempFor(ctx, expr->queryType(), target, typemod_none, FormatNatural);
}

void HqlCppTranslator::createTempFor(BuildCtx & ctx, ITypeInfo * _exprType, CHqlBoundTarget & target, typemod_t modifier, ExpressionFormat format)
{
    Owned<ITypeInfo> exprType = makeModifier(LINK(_exprType->queryPromotedType()), modifier);

    type_t tc = exprType->getTypeCode();
    switch (tc)
    {
    case type_array:
    case type_table:
    case type_groupedtable:
        {
            if (recordRequiresSerialization(::queryRecord(exprType)) || hasLinkCountedModifier(_exprType))
            {
                assertex(format != FormatBlockedDataset);
                format = FormatLinkedDataset;
            }
            else if (options.tempDatasetsUseLinkedRows && (format == FormatNatural))
                format = FormatLinkedDataset;
            break;
        }
    }

    if (hasLinkCountedModifier(exprType))
    {
        if (format == FormatNatural)
            format = FormatLinkedDataset;
        else if (format == FormatBlockedDataset)
            exprType.setown(setLinkCountedAttr(exprType, false));
    }
    else
    {
        if (format == FormatNatural)
            format = FormatBlockedDataset;
        else if ((format == FormatLinkedDataset) || (format == FormatArrayDataset))
            exprType.setown(setLinkCountedAttr(exprType, true));
    }

    size32_t size = exprType->getSize();
    if (size == UNKNOWN_LENGTH)
    {
        switch (tc)
        {
        case type_string:
        case type_data:
        case type_qstring:
        case type_unicode:
        case type_utf8:
            {
                OwnedITypeInfo lenType = makeModifier(LINK(sizetType), modifier);
                target.expr.setown(createWrapperTemp(ctx, exprType, modifier));
                target.length.setown(ctx.getTempDeclare(lenType, NULL));
                break;
            }
        case type_varstring:
        case type_varunicode:
            {
                target.expr.setown(createWrapperTemp(ctx, exprType, modifier));
                break;
            }
        case type_set:
        case type_array:
        case type_table:
        case type_groupedtable:
            break;
        default:
            UNIMPLEMENTED;
        }
    }
    else if (size > MAX_SIMPLE_VAR_SIZE)
    {
        switch (tc)
        {
        case type_string:
        case type_data:
        case type_qstring:
        case type_unicode:
        case type_varstring:
        case type_varunicode:
        case type_utf8:
            {
                target.expr.setown(createWrapperTemp(ctx, exprType, modifier));
                break;
            }
        }
    }


    switch (tc)
    {
    case type_set:
        {
            OwnedITypeInfo isAllType = makeModifier(LINK(boolType), modifier);
            target.isAll.setown(ctx.getTempDeclare(isAllType, NULL));
        }
        //fall through
    case type_array:
    case type_table:
    case type_groupedtable:
        {
            OwnedITypeInfo lenType = makeModifier(LINK(sizetType), modifier);
            if ((modifier != typemod_member) && ctx.queryMatchExpr(queryConditionalRowMarker()))
                ctx.setNextPriority(BuildCtx::OutermostScopePrio);
            target.expr.setown(createWrapperTemp(ctx, exprType, modifier));
            if (isArrayRowset(exprType))
            {
                //A bit of a hack, but the cleanest I could come up with... really access to the count member should be wrapped in
                //member functions, but getting them created needs a whole new level of complication (probably moving out out of hqlwcpp)
                StringBuffer name;
                target.expr->toString(name).append(".count");
                target.count.setown(createVariable(name, LINK(lenType)));
            }
            else
                target.length.setown(ctx.getTempDeclare(lenType, NULL));
            break;
        }
    }

    if (!target.expr)
    {
        target.expr.setown(ctx.getTempDeclare(exprType, NULL));
    }
}


void HqlCppTranslator::buildTempExpr(BuildCtx & ctx, BuildCtx & declareCtx, CHqlBoundTarget & tempTarget, IHqlExpression * expr, ExpressionFormat format, bool ignoreSetAll)
{
    if (options.addLocationToCpp)
    {
        IHqlExpression * location = queryLocation(expr);
        if (location)
        {
            StringBuffer s;
            s.append("// ").append(location->querySourcePath()->str()).append("(").append(location->getStartLine()).append(",").append(location->getStartColumn()).append(")  ").append(expr->queryName());
            ctx.addQuoted(s);
        }
        else if (expr->queryName())
        {
            StringBuffer s;
            s.append("// ").append(expr->queryName());
            ctx.addQuoted(s);
        }
    }

    typemod_t modifier = (&ctx != &declareCtx) ? typemod_member : typemod_none;
    OwnedITypeInfo type = makeModifier(expr->getType(), modifier);
    BuildCtx subctx(ctx);
    switch (type->getTypeCode())
    {
    case type_row:
        {
            Owned<BoundRow> tempRow = declareTempRow(declareCtx, subctx, expr);
            IHqlStmt * stmt = subctx.addGroup();
            stmt->setIncomplete(true);

            Owned<BoundRow> rowBuilder = createRowBuilder(subctx, tempRow);
            Owned<IReferenceSelector> createdRef = createReferenceSelector(rowBuilder);
            buildRowAssign(subctx, createdRef, expr);
            finalizeTempRow(subctx, tempRow, rowBuilder);

            stmt->setIncomplete(false);
            stmt->mergeScopeWithContainer();
            tempTarget.expr.set(tempRow->queryBound());
            
            ctx.associate(*tempRow);
            break;
        }
        break;
    case type_table:
    case type_groupedtable:
        {
            createTempFor(declareCtx, type, tempTarget, modifier, format);
            IHqlStmt * stmt = subctx.addGroup();
            stmt->setIncomplete(true);
            buildDatasetAssign(subctx, tempTarget, expr);
            stmt->setIncomplete(false);
            stmt->mergeScopeWithContainer();
            break;
        }
    default:
        {
            createTempFor(declareCtx, type, tempTarget, modifier, format);
            if (ignoreSetAll)
                tempTarget.isAll.clear();
            IHqlStmt * stmt = subctx.addGroup();
            stmt->setIncomplete(true);
            buildExprAssign(subctx, tempTarget, expr);
            stmt->setIncomplete(false);
            stmt->mergeScopeWithContainer();
            break;
        }
    }
}


void HqlCppTranslator::buildTempExpr(BuildCtx & ctx, IHqlExpression * expr, CHqlBoundExpr & tgt, ExpressionFormat format)
{
    node_operator op = expr->getOperator();
    if (op == no_alias)
    {
        doBuildExprAlias(ctx, expr, &tgt);
        return;
    }

    if (isCast(expr))
    {
        ITypeInfo * exprType = expr->queryType();
        if (exprType->getStringLen() == UNKNOWN_LENGTH)
        {
            unsigned bestLen = getBestLengthEstimate(expr);
            if (bestLen != UNKNOWN_LENGTH)
            {
                IHqlExpression * uncast = expr->queryChild(0);
                Owned<ITypeInfo> stretchedType = getStretchedType(bestLen, exprType);
                OwnedHqlExpr castExpr = ensureExprType(uncast, stretchedType);
                buildTempExpr(ctx, castExpr, tgt, format);
                ctx.associateExpr(expr, tgt);
                return;
            }
        }
    }

    BuildCtx bestctx(ctx);
    if (expr->isPure() && ctx.getMatchExpr(expr, tgt))
        return;

    switch (expr->getOperator())
    {
    case no_variable:
        tgt.expr.set(expr);
        return;
    case no_translated:
        {
            if (!expr->queryChild(1))
            {
                IHqlExpression * value = expr->queryChild(0);
                if (value->getOperator() == no_variable)
                {
                    tgt.expr.set(value);
                    return;
                }
            }
            break;
        }
    case no_id2blob:
        buildExpr(ctx, expr, tgt);
        return;
    case no_externalcall:
        if (format == FormatNatural && expr->isDataset())
            format = hasLinkCountedModifier(expr->queryType()) ? FormatLinkedDataset : FormatBlockedDataset;
        break;
    }

    LoopInvariantHelper helper;
    if (options.optimizeLoopInvariant)
        helper.getBestContext(bestctx, expr);

    bool canBeAll = canSetBeAll(expr);
    CHqlBoundTarget tempTarget;
    buildTempExpr(bestctx, bestctx, tempTarget, expr, format, !canBeAll);
    tgt.setFromTarget(tempTarget);

    bestctx.associateExpr(expr, tgt);
}

void HqlCppTranslator::buildExprViaTypedTemp(BuildCtx & ctx, IHqlExpression * expr, CHqlBoundExpr & tgt, ITypeInfo * type)
{
    OwnedHqlExpr cast = createValue(no_implicitcast, LINK(type), LINK(expr));
    if (cast->isPure() && ctx.getMatchExpr(cast, tgt))
        return;

    LoopInvariantHelper helper;
    BuildCtx bestctx(ctx);
    if (options.optimizeLoopInvariant)
        helper.getBestContext(bestctx, expr);

    CHqlBoundTarget tempTarget;
    createTempFor(bestctx, type, tempTarget, typemod_none, FormatNatural);
    buildExprAssign(bestctx, tempTarget, expr);
    tgt.setFromTarget(tempTarget);
    if (cast->isPure())
        bestctx.associateExpr(cast, tgt);
}


void HqlCppTranslator::buildExprEnsureType(BuildCtx & ctx, IHqlExpression * expr, CHqlBoundExpr & tgt, ITypeInfo * type)
{
    if (queryUnqualifiedType(expr->queryType()) != queryUnqualifiedType(type))
        buildExprViaTypedTemp(ctx, expr, tgt, type);
    else
        buildExpr(ctx, expr, tgt);
}


AliasKind HqlCppTranslator::doBuildAliasValue(BuildCtx & ctx, IHqlExpression * value, CHqlBoundExpr & tgt)
{
    //can happen when this is called for non no_alias arguments
    if (value->getOperator() == no_alias)
        value = value->queryChild(0);
    EvalContext * instance = queryEvalContext(ctx);
    if (instance)
        return instance->evaluateExpression(ctx, value, tgt, true);
    expandAliases(ctx, value);
    buildTempExpr(ctx, value, tgt);
    return RuntimeAlias;
}


void HqlCppTranslator::doBuildExprAlias(BuildCtx & ctx, IHqlExpression * expr, CHqlBoundExpr * tgt)
{
    //MORE These will be declared in a different context later.
    IHqlExpression * value = expr->queryChild(0);
    assertex(value->getOperator() != no_alias);

    //The second half of this test could cause aliases to be duplicated, but has the significant effect of reducing the amount of data that is serialised.
    //so far on my examples it does the latter, but doesn't seem to cause the former
    if (expr->hasProperty(localAtom) || (insideOnCreate(ctx) && !expr->hasProperty(globalAtom)))
    {
        expandAliases(ctx, value);
        if (tgt)
            buildTempExpr(ctx, value, *tgt);
        else
        {
            CHqlBoundExpr bound;
            buildTempExpr(ctx, value, bound);
        }
    }
    else
    {
        if (tgt)
            doBuildAliasValue(ctx, value, *tgt);
        else
        {
            CHqlBoundExpr bound;
            doBuildAliasValue(ctx, value, bound);
        }
    }
}


void HqlCppTranslator::doBuildBoolAssign(BuildCtx & ctx, const CHqlBoundTarget & target, IHqlExpression * expr)
{
    if (requiresTemp(ctx, expr, true))
    {
        BuildCtx subctx(ctx);
        assignBound(subctx, target, queryBoolExpr(false));
        buildFilter(subctx, expr);
        assignBound(subctx, target, queryBoolExpr(true));
    }
    else
    {
        CHqlBoundExpr temp;
        buildCachedExpr(ctx, expr, temp);
        assign(ctx, target, temp);
    }
}

void HqlCppTranslator::doBuildExprAssign(BuildCtx & ctx, const CHqlBoundTarget & target, IHqlExpression * expr)
{
    CHqlBoundExpr temp;
    buildExpr(ctx, expr, temp);
    assign(ctx, target, temp);
}

void HqlCppTranslator::ensureSimpleExpr(BuildCtx & ctx, CHqlBoundExpr & tgt)
{
    if (!isSimpleTranslatedExpr(tgt.expr))
    {
        OwnedHqlExpr bound = tgt.getTranslatedExpr();
        buildTempExpr(ctx, bound, tgt);
    }
}

IHqlExpression * HqlCppTranslator::ensureSimpleTranslatedExpr(BuildCtx & ctx, IHqlExpression * expr)
{
    if (isSimpleTranslatedExpr(expr))
        return LINK(expr);

    OwnedHqlExpr translated = createTranslated(expr);
    CHqlBoundExpr bound;
    buildTempExpr(ctx, translated, bound);
    return LINK(bound.expr);
}

void HqlCppTranslator::ensureHasAddress(BuildCtx & ctx, CHqlBoundExpr & tgt)
{
    IHqlExpression * expr = tgt.expr;
    node_operator op = expr->getOperator();

    switch (op)
    {
    case no_deref:
    case no_variable:
        break;
    default:
        if (!isTypePassedByAddress(expr->queryType()))
        {
            OwnedHqlExpr bound = tgt.getTranslatedExpr();
            buildTempExpr(ctx, bound, tgt);
        }
        break;
    }
}

//---------------------------------------------------------------------------

bool optimizeVarStringCompare(node_operator op, const CHqlBoundExpr & lhs, const CHqlBoundExpr & rhs, CHqlBoundExpr & tgt)
{
    IHqlExpression * rhsExpr = rhs.expr;
    if ((rhsExpr->getOperator() == no_constant) && (rhsExpr->queryType()->getStringLen() == 0))
    {
        if ((op == no_eq) || (op == no_ne))
        {
            tgt.expr.setown(createValue(op, LINK(boolType), createValue(no_deref, makeCharType(), lhs.expr.getLink()), getZero()));
            return true;
        }
    }
    return false;
}

void HqlCppTranslator::doBuildExprSetCompareAll(BuildCtx & ctx, IHqlExpression * set, CHqlBoundExpr & tgt, bool invert)
{
    Owned<IHqlCppSetCursor> cursor = createSetSelector(ctx, set);

    cursor->buildIsAll(ctx, tgt);
    if (invert)
        tgt.expr.setown(getInverse(tgt.expr));
}


void HqlCppTranslator::doBuildExprSetCompareNone(BuildCtx & ctx, IHqlExpression * set, CHqlBoundExpr & tgt, bool invert)
{
    Owned<IHqlCppSetCursor> cursor = createSetSelector(ctx, set);

    cursor->buildExists(ctx, tgt);
    if (!invert)
        tgt.expr.setown(getInverse(tgt.expr));
}

bool HqlCppTranslator::doBuildExprSetCompare(BuildCtx & ctx, IHqlExpression * expr, CHqlBoundExpr & tgt)
{
    //Special case comparison against all and null set.  All other work goes through a the order code.
    node_operator exprOp = expr->getOperator();
    if ((exprOp == no_eq) || (exprOp == no_ne))
    {
        OwnedHqlExpr left = normalizeListCasts(expr->queryChild(0));
        OwnedHqlExpr right = normalizeListCasts(expr->queryChild(1));

        if (right->getOperator() == no_all)
            doBuildExprSetCompareAll(ctx, left, tgt, exprOp==no_ne);
        else if (left->getOperator() == no_all)
            doBuildExprSetCompareAll(ctx, right, tgt, exprOp==no_ne);
        else if (isNullList(right))
            doBuildExprSetCompareNone(ctx, left, tgt, exprOp==no_ne);
        else if (isNullList(left))
            doBuildExprSetCompareNone(ctx, right, tgt, exprOp==no_ne);
        else
            return false;
        return true;
    }
    return false;
}

IHqlExpression * HqlCppTranslator::convertBoundStringToChar(const CHqlBoundExpr & bound)
{
    OwnedHqlExpr element = getElementPointer(bound.expr);
    Owned<ITypeInfo> charType = makeCharType(true);
    switch (element->getOperator())
    {
    case no_constant:
        {
            IValue * value = element->queryValue();
            return createConstant(value->castTo(charType));
        }
    case no_address:
        return LINK(element->queryChild(0));
    }
    return createValue(no_deref, charType.getClear(), element.getClear());
}


void HqlCppTranslator::doBuildExprCompare(BuildCtx & ctx, IHqlExpression * expr, CHqlBoundExpr & tgt)
{
    IHqlExpression * left = expr->queryChild(0);
    IHqlExpression * right = expr->queryChild(1);
    ITypeInfo * leftType = left->queryType()->queryPromotedType();
    ITypeInfo * rightType = right->queryType()->queryPromotedType();
    assertex(areTypesComparable(leftType,rightType));
    OwnedHqlExpr orderExpr;
    CHqlBoundExpr lhs, rhs;
    node_operator compareOp = expr->getOperator();

    type_t tc = leftType->getTypeCode();
    switch (tc)
    {
        case type_string:
        case type_data:
        case type_qstring:
            {
                OwnedHqlExpr simpleLeft = getSimplifyCompareArg(left);
                OwnedHqlExpr simpleRight = getSimplifyCompareArg(right);

                HqlExprArray args;
                buildCachedExpr(ctx, simpleLeft, lhs);
                buildCachedExpr(ctx, simpleRight, rhs);

                //update types - lengths may be constant by now..
                leftType = lhs.queryType();
                rightType = rhs.queryType();
                _ATOM func = queryStrCompareFunc(leftType);
                //MORE: Move blank string compare here?
                if (lhs.length || rhs.length || needVarStringCompare(leftType, rightType))
                {
                    args.append(*getBoundLength(lhs));
                    args.append(*getElementPointer(lhs.expr));

                    if (func == compareStrStrAtom && isBlankString(rhs.expr))
                    {
                        func = compareStrBlankAtom;
                    }
                    else
                    {
                        args.append(*getBoundLength(rhs));
                        args.append(*getElementPointer(rhs.expr));
                    }
                    orderExpr.setown(bindTranslatedFunctionCall(func, args));
                }
                else if (options.optimizeString1Compare &&
                    (tc == type_string || tc == type_data) && (leftType->getSize() == 1) &&
                    ((compareOp == no_eq) || (compareOp == no_ne)))
                {
                    //Optimize equality/non equality of a single character string.  
                    //Not done for > etc because of potential issues with signed/unsigned chars
                    args.append(*convertBoundStringToChar(lhs));
                    args.append(*convertBoundStringToChar(rhs));
                    tgt.expr.setown(createValue(compareOp, makeBoolType(), args));
                    return;
                }
                else
                {
                    args.append(*getElementPointer(lhs.expr));
                    args.append(*getElementPointer(rhs.expr));
                    args.append(*getSizetConstant(leftType->getSize()));
                
                    orderExpr.setown(bindTranslatedFunctionCall(memcmpAtom, args));
                }
                break;
            }
        case type_unicode:
            {
                OwnedHqlExpr simpleLeft = LINK(left);//getSimplifyCompareArg(left);
                OwnedHqlExpr simpleRight = LINK(right);//getSimplifyCompareArg(right);

                HqlExprArray args;
                buildCachedExpr(ctx, simpleLeft, lhs);
                buildCachedExpr(ctx, simpleRight, rhs);

                assertex(haveCommonLocale(leftType, rightType));
                char const * locale = getCommonLocale(leftType, rightType)->str();
                args.append(*getBoundLength(lhs));
                args.append(*getElementPointer(lhs.expr));
                args.append(*getBoundLength(rhs));
                args.append(*getElementPointer(rhs.expr));
                args.append(*createConstant(locale));
                orderExpr.setown(bindTranslatedFunctionCall(compareUnicodeUnicodeAtom, args));
                break;
            }
        case type_varunicode:
            {
                HqlExprArray args;
                buildCachedExpr(ctx, left, lhs);
                buildCachedExpr(ctx, right, rhs);
                assertex(haveCommonLocale(leftType, rightType));
                char const * locale = getCommonLocale(leftType, rightType)->str();
                args.append(*getElementPointer(lhs.expr));
                args.append(*getElementPointer(rhs.expr));
                args.append(*createConstant(locale));
                orderExpr.setown(bindTranslatedFunctionCall(compareVUnicodeVUnicodeAtom, args));
                break;
            }
        case type_utf8:
            {
                OwnedHqlExpr simpleLeft = LINK(left);//getSimplifyCompareArg(left);
                OwnedHqlExpr simpleRight = LINK(right);//getSimplifyCompareArg(right);

                HqlExprArray args;
                buildCachedExpr(ctx, simpleLeft, lhs);
                buildCachedExpr(ctx, simpleRight, rhs);

                assertex(haveCommonLocale(leftType, rightType));
                char const * locale = getCommonLocale(leftType, rightType)->str();
                args.append(*getBoundLength(lhs));
                args.append(*getElementPointer(lhs.expr));
                args.append(*getBoundLength(rhs));
                args.append(*getElementPointer(rhs.expr));
                args.append(*createConstant(locale));
                orderExpr.setown(bindTranslatedFunctionCall(compareUtf8Utf8Atom, args));
                break;
            }
        case type_varstring:
            {
                HqlExprArray args;
                buildCachedExpr(ctx, left, lhs);
                buildCachedExpr(ctx, right, rhs);

                //optimize comparison against null string.
                if (optimizeVarStringCompare(compareOp, lhs, rhs, tgt) || optimizeVarStringCompare(compareOp, rhs, lhs, tgt))
                    return;

                args.append(*getElementPointer(lhs.expr));
                args.append(*getElementPointer(rhs.expr));
                
                orderExpr.setown(bindTranslatedFunctionCall(strcmpAtom, args));
                break;
            }
        case type_decimal:
            {
                HqlExprArray args;
                buildCachedExpr(ctx, left, lhs);
                buildCachedExpr(ctx, right, rhs);
                if (!isPushed(lhs) && !isPushed(rhs) && isSameBasicType(leftType, rightType))
                {
                    args.append(*getSizetConstant(leftType->getSize()));
                    args.append(*getPointer(lhs.expr));
                    args.append(*getPointer(rhs.expr));
                    orderExpr.setown(bindTranslatedFunctionCall(leftType->isSigned() ? DecCompareDecimalAtom : DecCompareUDecimalAtom, args));
                }
                else
                {
                    bool pushedLhs = ensurePushed(ctx, lhs);
                    bool pushedRhs = ensurePushed(ctx, rhs);

                    //NB: Arguments could be pushed in opposite order 1 <=> x *2
                    if (pushedLhs && !pushedRhs)
                        orderExpr.setown(bindTranslatedFunctionCall(DecDistinctRAtom, args));
                    else
                        orderExpr.setown(bindTranslatedFunctionCall(DecDistinctAtom, args));
                }
                break;
            }
        case type_set:
        case type_array:
            {
                if (doBuildExprSetCompare(ctx, expr, tgt))
                    return;
                //fallthrough....
            }
        case type_table:
        case type_groupedtable:
        case type_row:
        case type_record:
            {
                IHqlExpression * orderExpr = createValue(no_order, LINK(signedType), LINK(left), LINK(right));
                OwnedHqlExpr cmpExpr = createBoolExpr(compareOp, orderExpr, createConstant(signedType->castFrom(true, 0)));
                buildExpr(ctx, cmpExpr, tgt);
                return;
            }
        case type_swapint:
        case type_packedint:
            {
                Owned<ITypeInfo> type = makeIntType(leftType->getSize(), leftType->isSigned());
                IHqlExpression * intLeft = createValue(no_implicitcast, type.getLink(), LINK(left));
                IHqlExpression * intRight = createValue(no_implicitcast, type.getLink(), LINK(right));
                OwnedHqlExpr transformed = createValue(compareOp, makeBoolType(), intLeft, intRight);
                doBuildPureSubExpr(ctx, transformed, tgt);
                return;
            }
        default:
            doBuildPureSubExpr(ctx, expr, tgt);
            return;
    }

    tgt.expr.setown(createValue(compareOp, LINK(boolType), LINK(orderExpr), createConstant(orderExpr->queryType()->castFrom(true, 0))));
}


//---------------------------------------------------------------------------
//-- no_and --

void HqlCppTranslator::doBuildFilterToTarget(BuildCtx & ctx, const CHqlBoundTarget & isOk, HqlExprArray & conds, bool invert)
{
    LinkedHqlExpr test = isOk.expr;
    if (invert)
        test.setown(createValue(no_not, makeBoolType(), LINK(test)));

    unsigned max = conds.ordinality();
    unsigned curIndex = 0;
    for (unsigned outer =0; curIndex < max; outer += MAX_NESTED_CASES)
    {
        BuildCtx subctx(ctx);
        if (outer != 0)
            subctx.addFilter(test);
        buildExprAssign(subctx, isOk, queryBoolExpr(false ^ invert));
        doBuildFilterNextAndRange(subctx, curIndex, MAX_NESTED_CASES, conds);
        buildExprAssign(subctx, isOk, queryBoolExpr(true ^ invert));
    }
}

void HqlCppTranslator::doBuildFilterAnd(BuildCtx & ctx, IHqlExpression * expr)
{
    HqlExprArray conds;
    expr->unwindList(conds, no_and);

    //Estimate the depth generated by the conditions - it may be wrong because
    //aliases generated in outer levels can stop temporaries being required later.
    unsigned numConds = conds.ordinality();
    unsigned depthEstimate = 1;
    for  (unsigned i=1; i < numConds; i++)
        if (requiresTemp(ctx, &conds.item(i), true))
            depthEstimate++;

    if (depthEstimate < MAX_NESTED_CASES)
    {
        unsigned curIndex = 0;
        doBuildFilterNextAndRange(ctx, curIndex, numConds, conds);
    }
    else
    {
        CHqlBoundTarget isOk;
        createTempFor(ctx, expr, isOk);
        doBuildFilterToTarget(ctx, isOk, conds, false);
        ctx.addFilter(isOk.expr);
    }
}


void HqlCppTranslator::doBuildFilterNextAndRange(BuildCtx & ctx, unsigned & curIndex, unsigned maxIterations, HqlExprArray & conds)
{
    unsigned max = conds.ordinality();
    for (unsigned i=0; (i < maxIterations) && (curIndex != max); i++)
    {
        unsigned last;
        expandAliases(ctx, &conds.item(curIndex));
        for (last = curIndex+1; last < max; last++)
            if (requiresTemp(ctx, &conds.item(last), true))
                break;
        doBuildFilterAndRange(ctx, curIndex, last, conds);
        curIndex = last;
    }
}

void HqlCppTranslator::doBuildFilterAndRange(BuildCtx & ctx, unsigned first, unsigned last, HqlExprArray & conds)
{
    if (first+1 == last)
        buildFilter(ctx, &conds.item(first));
    else
    {
        HqlExprArray args;
        for (unsigned k = first; k < last; k++)
            args.append(OLINK(conds.item(k)));
        OwnedHqlExpr test = createValue(no_and, makeBoolType(), args);
        buildFilterViaExpr(ctx, test);
    }
}


void HqlCppTranslator::doBuildAssignAnd(BuildCtx & ctx, const CHqlBoundTarget & target, IHqlExpression * expr, bool invert)
{
    HqlExprArray conds;
    expr->unwindList(conds, no_and);

    doBuildFilterToTarget(ctx, target, conds, invert);
}

void HqlCppTranslator::doBuildAssignOr(BuildCtx & ctx, const CHqlBoundTarget & target, IHqlExpression * expr)
{
    BuildCtx subctx(ctx);
    HqlExprArray conds;
    expr->unwindList(conds, no_or);

    unsigned first = 0;
    unsigned max = conds.ordinality();
    while (first < max)
    {
        unsigned last = first+1;
        //special case no_in because it always creates a temporary, so may as well assign on its own
        if (conds.item(first).getOperator() != no_in)
        {
            for (; last < max; last++)
            {
                IHqlExpression & cur = conds.item(last);
                if ((cur.getOperator() == no_in) || requiresTemp(subctx, &cur, true))
                    break;
            }
        }

        if (first != 0 || last != max)
        {
            if (last != first+1)
            {
                OwnedHqlExpr left = createBalanced(no_or, queryBoolType(), conds, first, last);
                doBuildExprAssign(subctx, target, left);
            }
            else
                buildExprAssign(subctx, target, &conds.item(first));

            if (last != max)
            {
                OwnedHqlExpr inverse = getInverse(target.expr);
                subctx.addFilter(inverse);
            }
        }
        else
        {
            doBuildExprAssign(subctx, target, expr);
        }
        first = last;
    }
}

//---------------------------------------------------------------------------
//-- no_case --

void HqlCppTranslator::doBuildCaseInfo(IHqlExpression * expr, HqlCppCaseInfo & info)
{
    unsigned maxMaps = expr->numChildren()-1;
    unsigned index = 0;
    if (expr->getOperator() == no_case)
    {
        info.setCond(expr->queryChild(0));
        index++;
    }
    
    for (;index<maxMaps;index++)
        info.addPair(expr->queryChild(index));

    info.setDefault(expr->queryChild(maxMaps));
}

void HqlCppTranslator::doBuildInCaseInfo(IHqlExpression * expr, HqlCppCaseInfo & info, IHqlExpression * normalized)
{
    bool valueIfMatch = (expr->getOperator() == no_in);
    HqlExprArray args;
    LinkedHqlExpr values = normalized;
    if (!normalized)
        values.setown(normalizeListCasts(expr->queryChild(1)));

    info.setCond(expr->queryChild(0));
    cvtInListToPairs(args, values, valueIfMatch);
    info.addPairs(args);
    info.setDefault(queryBoolExpr(!valueIfMatch));
}


void HqlCppTranslator::doBuildAssignInStored(BuildCtx & ctx, const CHqlBoundTarget & target, IHqlExpression * expr)
{
    OwnedHqlExpr values = normalizeListCasts(expr->queryChild(1));
    CHqlBoundExpr bound;
    if (values->isPure())
        ctx.getMatchExpr(values, bound);

    if (options.optimizeLoopInvariant && !bound.expr)
    {
        if (values->getOperator() == no_createset)
        {
            IHqlExpression * ds = values->queryChild(0);
            //MORE: This is a special case - it should check if the dataset can be iterated without any projection
            switch (ds->getOperator())
            {
            case no_select:
            case no_rows:
                break;
            default:
                {
                    //Evaluate the dataset into a temporary - the iterator will match it up later
                    //Evaluating the dataset instead of the set since this is likely to avoid a memcpy with link counted rows enabled.
                    LoopInvariantHelper helper;
                    BuildCtx bestctx(ctx);
                    if (helper.getBestContext(bestctx, ds))
                    {
                        CHqlBoundExpr temp;
                        buildTempExpr(ctx, ds, temp);
                    }
                    break;
                }
            }
        }
        else
        {
            LoopInvariantHelper helper;
            BuildCtx bestctx(ctx);
            if (helper.getBestContext(bestctx, values))
            {
                //Unfortunately this will do strength reduction again, but shouldn't take too long.
                buildTempExpr(bestctx, values, bound);
            }
        }
    }

    if (bound.expr)
        values.setown(bound.getTranslatedExpr());

    bool valueIfMatch = (expr->getOperator() == no_in);
    ITypeInfo * elementType = values->queryType()->queryChildType();
    Owned<ITypeInfo> compareType = getPromotedECLCompareType(expr->queryChild(0)->queryType(), elementType);

    CHqlBoundExpr boundSearch;
    OwnedHqlExpr castSearch = ensureExprType(expr->queryChild(0), compareType);

    BuildCtx subctx(ctx);
    Owned<IHqlCppSetCursor> cursor = createSetSelector(ctx, values);

    CHqlBoundExpr isAll;
    cursor->buildIsAll(ctx, isAll);
    if (isAll.expr->queryValue())
    {
        if (isAll.expr->queryValue()->getBoolValue())
        {
            buildExprAssign(subctx, target, queryBoolExpr(valueIfMatch));
            //If this inverted the test we would need to do more.
            return;
        }
    }
    else
    {
        IHqlStmt * stmt = subctx.addFilter(isAll.expr);
        buildExprAssign(subctx, target, queryBoolExpr(valueIfMatch));
        subctx.selectElse(stmt);
    }
    //result = false/true
    buildSimpleExpr(subctx, castSearch, boundSearch);
    buildExprAssign(subctx, target, queryBoolExpr(!valueIfMatch));
    
    //iterate through the set
    bool needToBreak = !cursor->isSingleValued();
    CHqlBoundExpr boundCurElement;
    cursor->buildIterateLoop(subctx, boundCurElement, needToBreak);
    OwnedHqlExpr curElement = boundCurElement.getTranslatedExpr();
    OwnedHqlExpr castCurElement = ensureExprType(curElement, compareType);

    //if match then
    OwnedHqlExpr matchTest = createValue(no_eq, makeBoolType(), boundSearch.getTranslatedExpr(), LINK(castCurElement));
    buildFilter(subctx, matchTest);

    //result = true/false + break loop.
    buildExprAssign(subctx, target, queryBoolExpr(valueIfMatch));
    if (needToBreak)
        subctx.addBreak();
}

void HqlCppTranslator::doBuildAssignInCreateSet(BuildCtx & ctx, const CHqlBoundTarget & target, IHqlExpression * expr)
{
    IHqlExpression * setExpr = expr->queryChild(1);
    IHqlExpression * dataset = setExpr->queryChild(0);
    IHqlExpression * selected = setExpr->queryChild(1);

    bool valueIfMatch = (expr->getOperator() == no_in);
    ITypeInfo * elementType = setExpr->queryType()->queryChildType();
    Owned<ITypeInfo> compareType = getPromotedECLCompareType(expr->queryChild(0)->queryType(), elementType);

    CHqlBoundExpr boundSearch;
    OwnedHqlExpr castSearch = ensureExprType(expr->queryChild(0), compareType);

    //result = false/true
    buildSimpleExpr(ctx, castSearch, boundSearch);
    buildExprAssign(ctx, target, queryBoolExpr(!valueIfMatch));
    
    //iterate through the set
    bool needToBreak = !hasNoMoreRowsThan(dataset, 1);
    BuildCtx loopctx(ctx);
    buildDatasetIterate(loopctx, dataset, needToBreak);

    //if match then
    OwnedHqlExpr matchTest = createValue(no_eq, makeBoolType(), boundSearch.getTranslatedExpr(), ensureExprType(selected, compareType));
    buildFilter(loopctx, matchTest);

    //result = true/false + break loop.
    buildExprAssign(loopctx, target, queryBoolExpr(valueIfMatch));
    if (needToBreak)
        loopctx.addBreak();
}

void HqlCppTranslator::doBuildAssignIn(BuildCtx & ctx, const CHqlBoundTarget & target, IHqlExpression * expr)
{
    OwnedHqlExpr values = normalizeListCasts(expr->queryChild(1));

    node_operator op = expr->getOperator();
    bool valueIfMatch = (op == no_in);
    switch (values->getOperator())
    {
    case no_all:
        buildExprAssign(ctx, target, queryBoolExpr(valueIfMatch));
        break;
    case no_null:
        buildExprAssign(ctx, target, queryBoolExpr(!valueIfMatch));
        break;
    case no_list:
        {
            HqlCppCaseInfo info(*this);
            doBuildInCaseInfo(expr, info, values);
            info.buildAssign(ctx, target);
            break;
        }
    case no_createset:
        {
            //Look further at bug #52745.eclxml and what causes the poor code.
            //if (canIterateInline(&ctx, values->queryChild(0)))
            //  doBuildAssignInCreateSet(ctx, target, expr);
            //else
                doBuildAssignInStored(ctx, target, expr);
            break;
        }

#if 0
        //Possible optimizations, but I don't have any examples that would trigger them, so don't enable
    case no_if:
        {
            BuildCtx subctx(ctx);
            OwnedHqlExpr inLhs = createValue(expr->getOperator(), LINK(expr->queryChild(0)), LINK(values->queryChild(1)));
            OwnedHqlExpr inRhs = createValue(expr->getOperator(), LINK(expr->queryChild(0)), LINK(values->queryChild(2)));
            IHqlStmt * stmt = buildFilterViaExpr(subctx, values->queryChild(0));
            buildExprAssign(subctx, target, inLhs);
            subctx.selectElse(stmt);
            buildExprAssign(subctx, target, inRhs);
            break;
        }
    case no_addsets:
        if (op == no_in)
        {
            BuildCtx subctx(ctx);
            OwnedHqlExpr inLhs = createValue(expr->getOperator(), LINK(expr->queryChild(0)), LINK(values->queryChild(0)));
            OwnedHqlExpr inRhs = createValue(expr->getOperator(), LINK(expr->queryChild(0)), LINK(values->queryChild(1)));
            buildExprAssign(ctx, target, inLhs);
            OwnedHqlExpr test = getInverse(target.expr);
            subctx.addFilter(test);
            buildExprAssign(subctx, target, inRhs);
            break;
        }
#endif
        //fall through
    default:
        doBuildAssignInStored(ctx, target, expr);
        break;
    }
}


//---------------------------------------------------------------------------

void HqlCppTranslator::doBuildExprArith(BuildCtx & ctx, IHqlExpression * expr, CHqlBoundExpr & tgt)
{
    ITypeInfo * type = expr->queryType();
    switch (type->getTypeCode())
    {
    case type_decimal:
        {
            bindAndPush(ctx, expr->queryChild(0));
            bindAndPush(ctx, expr->queryChild(1));
            
            HqlExprArray args;
            _ATOM func;
            switch (expr->getOperator())
            {
            case no_add: func = DecAddAtom; break;
            case no_div: func = DecDivideAtom; break;
            case no_modulus: func = DecModulusAtom; break;
            case no_mul: func = DecMulAtom; break;
            case no_sub: func = DecSubAtom; break;
            default: UNIMPLEMENTED;
            }
            callProcedure(ctx, func, args);
            tgt.expr.setown(createValue(no_decimalstack, LINK(type)));
        }
        break;
    case type_swapint:
    case type_packedint:
        {
            //someone is being deliberately perverse....
            ITypeInfo * type = expr->queryType();
            ITypeInfo * intType = makeIntType(type->getSize(), type->isSigned());
            IHqlExpression * lhs = expr->queryChild(0);
            IHqlExpression * rhs = expr->queryChild(1);
            assertex(isSameBasicType(type, lhs->queryType()));
            assertex(isSameBasicType(type, rhs->queryType()));
            lhs = createValue(no_implicitcast, LINK(intType), LINK(lhs));
            rhs = createValue(no_implicitcast, LINK(intType), LINK(rhs));
            IHqlExpression * newExpr = createValue(expr->getOperator(), intType, lhs, rhs);
            OwnedHqlExpr castNewExpr = createValue(no_implicitcast, LINK(type), newExpr);
            buildExpr(ctx, castNewExpr, tgt);
            break;
        }
    default:
        doBuildPureSubExpr(ctx, expr, tgt);
        break;
    }
}

void HqlCppTranslator::doBuildExprAdd(BuildCtx & ctx, IHqlExpression * expr, CHqlBoundExpr & tgt)
{
    if (expr->queryType()->getTypeCode() != type_int)
    {
        doBuildExprArith(ctx, expr, tgt);
        return;
    }

    CHqlBoundExpr boundL, boundR;
    buildExpr(ctx, expr->queryChild(0), boundL);

    bool WORK_AROUND_GCC_CONDITION_BUG = (options.targetCompiler == GccCppCompiler);
    if (WORK_AROUND_GCC_CONDITION_BUG && expr->queryChild(1)->getOperator() == no_if)
        buildTempExpr(ctx, expr->queryChild(1), boundR);
    else
        buildExpr(ctx, expr->queryChild(1), boundR);

    tgt.expr.setown(adjustBoundIntegerValues(boundL.expr, boundR.expr, false));
}

//---------------------------------------------------------------------------

void HqlCppTranslator::doBuildExprNegate(BuildCtx & ctx, IHqlExpression * expr, CHqlBoundExpr & tgt)
{
    ITypeInfo * type = expr->queryType();
    switch (type->getTypeCode())
    {
    case type_decimal:
        {
            bindAndPush(ctx, expr->queryChild(0));
            HqlExprArray args;
            callProcedure(ctx, DecNegateAtom, args);
            tgt.expr.setown(createValue(no_decimalstack, LINK(type)));
        }
        break;
    case type_data:
    case type_qstring:
    case type_string:
    case type_varstring:
    case type_unicode:
    case type_varunicode:
    case type_utf8:
        throwError(HQLERR_MinusOnString);
    default:
        doBuildPureSubExpr(ctx, expr, tgt);
        break;
    }
}

//---------------------------------------------------------------------------

void HqlCppTranslator::doBuildExprRound(BuildCtx & ctx, IHqlExpression * expr, CHqlBoundExpr & tgt)
{
    IHqlExpression * arg = expr->queryChild(0);
    node_operator op = expr->getOperator();
    switch (arg->queryType()->getTypeCode())
    {
    case type_decimal:
        {
            bindAndPush(ctx, arg);
            HqlExprArray args;
            if (op == no_round)
            {
                IHqlExpression * places = queryRealChild(expr, 1);
                if (places)
                {
                    args.append(*LINK(places));
                    callProcedure(ctx, DecRoundToAtom, args);
                }
                else
                    callProcedure(ctx, DecRoundAtom, args);
            }
            else
                callProcedure(ctx, DecRoundUpAtom, args);
            assertex(expr->queryType()->getTypeCode() == type_decimal);
            tgt.expr.setown(createValue(no_decimalstack, expr->getType()));
        }
        break;
    default:
        {
            if (op == no_round)
            {
                if (queryRealChild(expr, 1))
                    doBuildExprSysFunc(ctx, expr, tgt, roundToAtom);
                else
                    doBuildExprSysFunc(ctx, expr, tgt, roundAtom);
            }
            else
                doBuildExprSysFunc(ctx, expr, tgt, roundupAtom);
            break;
        }
    }
}

//---------------------------------------------------------------------------

void HqlCppTranslator::doBuildExprAbs(BuildCtx & ctx, IHqlExpression * expr, CHqlBoundExpr & tgt)
{
    ITypeInfo * type = expr->queryType();
    switch (type->getTypeCode())
    {
    case type_decimal:
        {
            bindAndPush(ctx, expr->queryChild(0));
            
            HqlExprArray args;
            _ATOM func = DecAbsAtom;
            callProcedure(ctx, func, args);
            tgt.expr.setown(createValue(no_decimalstack, LINK(type)));
        }
        break;
    case type_int:
    case type_swapint:
    case type_packedint:
    case type_real:
        {
            CHqlBoundExpr temp;
            buildTempExpr(ctx, expr->queryChild(0), temp);

            ITypeInfo * type = expr->getType();
            IHqlExpression * cond = createValue(no_ge, temp.expr.getLink(), createConstant(type->castFrom(true, 0)));
            tgt.expr.setown(createValue(no_if, type, cond, temp.expr.getLink(), createValue(no_negate, LINK(type), temp.expr.getLink())));
        }
        break;
    default:
        buildExpr(ctx, expr->queryChild(0), tgt);
        break;
    }
}

//---------------------------------------------------------------------------

void HqlCppTranslator::doBuildAssignCatch(BuildCtx & ctx, const CHqlBoundTarget & target, IHqlExpression * expr)
{
    BuildCtx subctx(ctx);
    subctx.addGroup();

    BuildCtx tryctx(subctx);
    tryctx.addQuotedCompound("try");
    buildExprAssign(tryctx, target, expr->queryChild(0));

    BuildCtx catchctx(subctx);
    catchctx.addQuotedCompound("catch (IException * e)");
    IHqlExpression * exceptVar = associateLocalFailure(catchctx, "e");
    buildExprAssign(catchctx, target, expr->queryChild(1));

    HqlExprArray args;
    args.append(*LINK(exceptVar));
    callProcedure(catchctx, freeExceptionAtom, args);
}

//---------------------------------------------------------------------------
//-- no_externalcall --

IHqlExpression * getCastParameter(IHqlExpression * curParam, ITypeInfo * argType)
{
    type_t atc = argType->getTypeCode();

    //Remove a few unnecessary casts which clutter up the code/or make it less efficient
    if (isCast(curParam) && (curParam->queryType() == argType))
    {
        //If the following code is incorrect, the casts should get added back by the code that follows
        IHqlExpression * uncast = curParam->queryChild(0);

        //casts to larger size integers 
        if (atc == type_int)
            curParam = uncast;
        else if ((atc == type_unicode) && (uncast->queryType()->getTypeCode() == type_varunicode))
            curParam = uncast;
        else if ((atc == type_string) && (uncast->queryType()->getTypeCode() == type_varstring))
            curParam = uncast;
    }

    ITypeInfo * paramType = curParam->queryType();
    type_t ptc = paramType->getTypeCode();
    if ((atc != ptc) && !(atc == type_row && ptc == type_table))
    {
        switch (atc)
        {
        case type_unicode:
            if ((argType->getSize() == UNKNOWN_LENGTH) && (ptc == type_varunicode))
                return LINK(curParam);
            break;
        case type_string:
            if ((argType->getSize() == UNKNOWN_LENGTH) && 
                ((ptc == type_varstring) && (argType->queryCharset() == paramType->queryCharset())))
                return LINK(curParam);
            break;
        case type_row:
        case type_table:
        case type_groupedtable:
            {
                IHqlExpression * record = queryRecord(argType);
                if (record && (record->numChildren() != 0))
                {
                    //really need to project instead.
                    //return ensureExprType(curParam, argType);
                }
                return LINK(curParam);
            }
        }
        return ensureExprType(curParam, argType);
    }

    ITypeInfo * childType = argType->queryChildType();
    if (paramType->queryTypeBase() != argType)
    {
        size32_t argSize = argType->getSize();
        switch (atc)
        {
        case type_int:
        case type_real:
            if (argSize <= paramType->getSize())
                return ensureExprType(curParam, argType);
            break;
        case type_decimal:
            //Don't need explicit cast to cast between different sizes of these types...
            //Will be done automatically by the compiler.
            break;
        case type_unicode:
            //Don't need cast between different locales
            if ((argSize != paramType->getSize()) && (argSize != UNKNOWN_LENGTH))
            {
                Owned<ITypeInfo> modArgType = makeUnicodeType(argType->getStringLen(), curParam->queryType()->queryLocale());
                return ensureExprType(curParam, modArgType);
            }
            break;
        case type_varunicode:
            //Don't need cast between different locales
            if(argSize != paramType->getSize())
            {
                Owned<ITypeInfo> modArgType = makeVarUnicodeType(argType->getStringLen(), curParam->queryType()->queryLocale());
                return ensureExprType(curParam, modArgType);
            }
            break;
        case type_utf8:
            //Don't need cast between different locales
            if(argSize != paramType->getSize())
            {
                Owned<ITypeInfo> modArgType = makeUtf8Type(argType->getStringLen(), curParam->queryType()->queryLocale());
                return ensureExprType(curParam, modArgType);
            }
            break;
        case type_set:
            if (childType)
                return ensureExprType(curParam, argType);
            break;
        case type_table:
        case type_groupedtable:
        case type_row:
            {
                IHqlExpression * record = queryRecord(argType);
                if (record && (record->numChildren() != 0))
                {
                    //really need to project instead.
                    //return ensureExprType(curParam, argType);
                }
                break;
            }
        default:
            return ensureExprType(curParam, argType);
        }
    }

//  if (argType->queryCharset() != paramType->queryCharset())
//      return ensureExprType(curParam, argType);

    return LINK(curParam);
}

void HqlCppTranslator::normalizeBoundExpr(BuildCtx & ctx, CHqlBoundExpr & bound)
{
    bound.expr.setown(convertWrapperToPointer(bound.expr));
}

IHqlExpression * HqlCppTranslator::doBuildInternalFunction(IHqlExpression * funcdef)
{
    unsigned match = internalFunctions.find(*funcdef);
    if (match != NotFound)
        return LINK(&internalFunctionExternals.item(match));

    StringBuffer funcname;
    funcname.append("user").append(internalFunctions.ordinality()+1);
    if (options.debugGeneratedCpp)
        funcname.append("_").append(funcdef->queryName()).toLowerCase();

    HqlExprArray attrs;
    attrs.append(*createLocalAttribute());
    attrs.append(*createExprAttribute(entrypointAtom, createConstant(funcname)));

    HqlExprArray funcdefArgs;
    ITypeInfo * returnType = funcdef->queryType()->queryChildType();
    funcdefArgs.append(*createExternalReference(funcdef->queryName(), LINK(returnType), attrs));
    funcdefArgs.append(*LINK(queryFunctionParameters(funcdef)));
    unwindChildren(funcdefArgs, funcdef, 1);
    OwnedHqlExpr externalFuncdef = funcdef->clone(funcdefArgs);

    internalFunctions.append(*LINK(funcdef));
    internalFunctionExternals.append(*LINK(externalFuncdef));

    buildFunctionDefinition(externalFuncdef, funcdef);
    return LINK(externalFuncdef);
}

void HqlCppTranslator::doBuildCall(BuildCtx & ctx, const CHqlBoundTarget * tgt, IHqlExpression * expr, CHqlBoundExpr * result)
{
    if (result && expr->isPure() && ctx.getMatchExpr(expr, *result))
        return;

    LinkedHqlExpr funcdef;
    if (expr->getOperator() == no_externalcall)
    {
        funcdef.set(expr->queryExternalDefinition());
        assertex(funcdef);
        useFunction(funcdef);
    }
    else
    {
        IHqlExpression * def = expr->queryBody()->queryFunctionDefinition();
        assertex(def);
        funcdef.setown(doBuildInternalFunction(def));
    }

    IHqlExpression * external = funcdef->queryChild(0);
    IHqlExpression * formals = funcdef->queryChild(1);
    if (external->hasProperty(ctxmethodAtom))
        ensureContextAvailable(ctx);
    if (external->hasProperty(gctxmethodAtom) || external->hasProperty(globalContextAtom))
    {
        if (!ctx.queryMatchExpr(globalContextMarkerExpr))
            throwError1(HQLERR_FuncNotInGlobalContext, external->queryName()->str());
    }

    unsigned maxArg = formals->numChildren();
    unsigned maxParam = expr->numChildren();
    bool returnByReference = false;
    bool returnMustAssign = false;
    HqlExprArray args;
    unsigned arg = 0;
    unsigned param;

    unsigned firstParam = 0;
    bool isMethod = external->hasProperty(methodAtom) || external->hasProperty(omethodAtom) ;
    bool newFormatSet = !external->hasProperty(oldSetFormatAtom);
    bool translateSetReturn = false;
    if (isMethod)
    {
        CHqlBoundExpr bound;
        buildExpr(ctx, expr->queryChild(firstParam++), bound);
        args.append(*bound.expr.getClear());
    }
    if (external->hasProperty(userMatchFunctionAtom))
    {
        //MORE: Test valid in this location...
        args.append(*createVariable("walker", makeBoolType()));
    }

    bool doneAssign = false;
    CHqlBoundExpr localBound;
    CHqlBoundTarget localTarget;
    Linked<BoundRow> resultRow;
    Linked<BoundRow> resultRowBuilder;
    Owned<ITypeInfo> targetType = tgt ? LINK(tgt->queryType()) : makeVoidType();
    ITypeInfo * retType = funcdef->queryType()->queryChildType();
    BoundRow * resultSelfCursor = NULL;
    switch (retType->getTypeCode())
    {
    case type_varstring:
    case type_varunicode:
        if (retType->getSize() == UNKNOWN_LENGTH)
        {
            if (hasConstModifier(retType))
                break;

            returnMustAssign = true;
            if (tgt && !tgt->isFixedSize())
            {
                doneAssign = true;
                localBound.expr.set(tgt->expr);
            }
            else
                localBound.expr.setown(createWrapperTemp(ctx, retType, typemod_none));
            break;
        }
        else
        {
            //fixed size strings => just pass pointer
            IHqlExpression * resultVar = ctx.getTempDeclare(retType, NULL);
            args.append(*getElementPointer(resultVar));
            localBound.expr.setown(resultVar);
        }
        returnByReference = true;
        break;
    case type_string:
    case type_data:
    case type_qstring:
    case type_unicode:
    case type_utf8:
        if (retType->getSize() == UNKNOWN_LENGTH)
        {
            OwnedHqlExpr lenVar;
            OwnedHqlExpr strVar;
            if (tgt && !tgt->isFixedSize())
            {
                doneAssign = true;
                strVar.set(tgt->expr);
                lenVar.set(tgt->length);
            }
            else
            {
                strVar.setown(createWrapperTemp(ctx, retType, typemod_none));
                lenVar.setown(ctx.getTempDeclare(sizetType, NULL));
            }
            args.append(*LINK(lenVar));
            args.append(*createValue(no_reference, strVar->getType(), LINK(strVar)));
            localBound.length.set(lenVar);
            localBound.expr.set(strVar);
        }
        else
        {
            //fixed size strings => just pass pointer
            if (tgt && tgt->isFixedSize() && targetType->queryPromotedType() == retType)
            {
                doneAssign = true;
                args.append(*getElementPointer(tgt->expr));
                localBound.expr.set(tgt->expr);
            }
            else
            {
                IHqlExpression * resultVar = ctx.getTempDeclare(retType, NULL);
                args.append(*getElementPointer(resultVar));
                localBound.expr.setown(resultVar);
            }
        }
        returnByReference = true;
        break;
    case type_set:
        {
            translateSetReturn = !newFormatSet;
            OwnedHqlExpr lenVar;
            OwnedHqlExpr strVar;
            OwnedHqlExpr isAll;
            if (tgt && !tgt->isFixedSize())
            {
                doneAssign = true;
                strVar.set(tgt->expr);
                if (translateSetReturn)
                    lenVar.setown(ctx.getTempDeclare(unsignedType, NULL));
                else
                    lenVar.set(tgt->length);
                assertex(tgt->isAll);
                isAll.set(tgt->isAll);
            }
            else
            {
                Owned<ITypeInfo> dataType = makeDataType(UNKNOWN_LENGTH);
                strVar.setown(createWrapperTemp(ctx, dataType, typemod_none));
                if (translateSetReturn)
                {
                    lenVar.setown(ctx.getTempDeclare(unsignedType, NULL));
                }
                else
                {
                    lenVar.setown(ctx.getTempDeclare(sizetType, NULL));
                    isAll.setown(ctx.getTempDeclare(boolType, NULL));
                }
            }
            if (newFormatSet)
                args.append(*LINK(isAll));
            args.append(*LINK(lenVar));
            args.append(*createValue(no_reference, strVar->getType(), LINK(strVar)));
            if (newFormatSet)
            {
                localBound.length.set(lenVar);
            }
            else
            {
                localBound.count.set(lenVar);
                if (isAll && !isAll->queryValue())
                    ctx.addAssign(isAll, queryBoolExpr(false));
            }

            localBound.isAll.set(isAll);
            localBound.expr.setown(createValue(no_implicitcast, makeReferenceModifier(LINK(retType)), LINK(strVar)));
            returnByReference = true;
            break;
        }
    case type_table:
    case type_groupedtable:
        {
            if (hasStreamedModifier(retType))
            {
                args.append(*createRowAllocator(ctx, ::queryRecord(retType)));
                break;
            }
            const CHqlBoundTarget * curTarget;
            if (tgt && !tgt->isFixedSize() && 
                (hasLinkCountedModifier(targetType) == hasLinkCountedModifier(retType)))
            {
                doneAssign = true;
                curTarget = tgt;
            }
            else
            {
                curTarget = &localTarget;
                ExpressionFormat format = (hasLinkCountedModifier(retType) ? FormatLinkedDataset : FormatBlockedDataset);
                createTempFor(ctx, retType, localTarget, typemod_none, format);
            }
            if (curTarget->count)
                args.append(*LINK(curTarget->count));
            if (curTarget->length)
                args.append(*LINK(curTarget->length));
            args.append(*createValue(no_reference, curTarget->expr->getType(), LINK(curTarget->expr)));
            if (hasLinkCountedModifier(retType) && hasNonNullRecord(retType) && getBoolProperty(external, allocatorAtom, true))
                args.append(*createRowAllocator(ctx, ::queryRecord(retType)));

            localBound.setFromTarget(*curTarget);
//          localBound.expr.setown(createValue(no_implicitcast, makeReferenceModifier(LINK(retType)), LINK(strVar)));
            returnByReference = true;
            break;
        }
    case type_row:
        {
            if (hasLinkCountedModifier(retType))
            {
                //Always assign link counted rows to a temporary (or the target) to ensure the are not leaked.
                returnMustAssign = true;
                if (tgt && hasLinkCountedModifier(targetType) && recordTypesMatch(targetType, retType))
                {
                    doneAssign = true;
                    localBound.expr.set(tgt->expr);
                }
                else
                    localBound.expr.setown(createWrapperTemp(ctx, retType, typemod_none));
            }
            else
            {
            //row, just pass pointer
                if (tgt && recordTypesMatch(targetType, retType) && !hasLinkCountedModifier(targetType))
                {
                    doneAssign = true;
                    args.append(*getPointer(tgt->expr));
                    localBound.expr.set(tgt->expr);
                }
                else
                {
                    resultRow.setown(declareTempRow(ctx, ctx, expr));
                    resultRowBuilder.setown(createRowBuilder(ctx, resultRow));
                    IHqlExpression * bound = resultRowBuilder->queryBound();
                    args.append(*getPointer(bound));
                    localBound.expr.set(bound);
                }
                returnByReference = true;
            }
            break;
        }
    case type_transform:
        {
            //Ugly, but target selector is passed in as the target
            assertex(tgt && tgt->expr);
            resultSelfCursor = resolveSelectorDataset(ctx, tgt->expr);
            assertex(resultSelfCursor);
            if (resultSelfCursor->queryBuilder())
                args.append(*LINK(resultSelfCursor->queryBuilder()));
            else
            {
                //Legacy support....
                assertex(!options.supportDynamicRows);
                OwnedHqlExpr rowExpr = getPointer(resultSelfCursor->queryBound());

                StringBuffer builderName;
                getUniqueId(builderName.append("b"));

                StringBuffer s;
                s.append("RtlStaticRowBuilder ").append(builderName).append("(");
                generateExprCpp(s, rowExpr).append(",").append(getMaxRecordSize(funcdef->queryRecord())).append(");");
                ctx.addQuoted(s);

                args.append(*createVariable(builderName, makeBoolType()));
            }
            returnByReference = true;
            doneAssign = true;
            break;
        }
    case type_array:
        UNIMPLEMENTED;
    }

    for (param = firstParam; param < maxParam; param++)
    {
        IHqlExpression * curParam = expr->queryChild(param);
        if (curParam->isAttribute())
            continue;

        if (arg >= maxArg)
        {
            PrintLog("Too many parameters passed to function '%s'", expr->queryName()->str());
            throwError1(HQLERR_TooManyParameters, expr->queryName()->str());
        }

        CHqlBoundExpr bound;
        IHqlExpression * curArg = formals->queryChild(arg);
        ITypeInfo * argType = curArg->queryType();

        OwnedHqlExpr castParam = getCastParameter(curParam, argType);

        type_t atc = argType->getTypeCode();
        switch (atc)
        {
        case type_table:
        case type_groupedtable:
            {
                ExpressionFormat format = queryNaturalFormat(argType);
                buildDataset(ctx, castParam, bound, format);
                break;
            }
        case type_row:
            {
                Owned<IReferenceSelector> selector = buildNewRow(ctx, castParam);
                selector->buildAddress(ctx, bound);
    //          buildExpr(ctx, castParam, bound);       // more this needs more work I think
                break;
            }
        case type_set:
            {
                ITypeInfo * elemType = argType->queryChildType();
                if (newFormatSet && elemType && (elemType->getTypeCode() != type_any) && (elemType != castParam->queryType()->queryChildType()))
                    buildExprEnsureType(ctx, castParam, bound, argType);
                else
                    buildExpr(ctx, castParam, bound);
                normalizeBoundExpr(ctx, bound);
                break;
            }
        case type_decimal:
            {
                buildSimpleExpr(ctx, castParam, bound);
                normalizeBoundExpr(ctx, bound);
                break;
            }
        default:
            {
                buildExpr(ctx, castParam, bound);
                normalizeBoundExpr(ctx, bound);
                break;
            }
        }

        bool done = false;
        switch (atc)
        {
        case type_string:
        case type_data:
        case type_qstring:
        case type_unicode:
        case type_utf8:
            if (argType->getSize() == UNKNOWN_LENGTH)
                args.append(*getBoundLength(bound));
            /*
            Ensure parameter is passed as non-const if the argument does not have const.
            if (!curArg->hasProperty(constAtom))//!argType->isConstantType())// && bound.queryType()->isConstantType())
                bound.expr.setown(createValue(no_cast, LINK(argType), LINK(bound.expr)));
                */
            break;
        case type_varstring:
        case type_varunicode:
            //MORE: pass length only if an out parameter
            break;
        case type_set:
            {
                if (newFormatSet)
                {
                    args.append(*bound.getIsAll());
                    args.append(*getBoundSize(bound));
                }
                else
                {
                    if (castParam->getOperator() == no_all)
                        throwError(HQLERR_AllPassedExternal);
                    args.append(*getBoundCount(bound));
                }
                break;
            }
        case type_table:
        case type_groupedtable:
            {
                if (isArrayRowset(argType))
                    args.append(*getBoundCount(bound));
                else
                    args.append(*getBoundSize(bound));
                bound.expr.setown(getPointer(bound.expr));
                break;
            }
        case type_array:
            UNIMPLEMENTED;
        }

        if (!done)
            args.append(*bound.expr.getClear());
        arg++;
    }

    if (arg < maxArg)
    {
        //MORE: Process default parameters...
        PrintLog("Not enough parameters passed to function '%s'", expr->queryName()->str());
        throwError1(HQLERR_TooFewParameters, expr->queryName()->str());
    }

    OwnedHqlExpr call = bindTranslatedFunctionCall(funcdef, args);

    //either copy the integral value across, or a var string to fixed string
    if (returnMustAssign)
    {
        ctx.addAssign(localBound.expr, call);
    }
    else if (resultSelfCursor)
    {
        OwnedHqlExpr sizeVar = ctx.getTempDeclare(unsignedType, call);
        OwnedHqlExpr sizeOfExpr = createSizeof(resultSelfCursor->querySelector());
        ctx.associateExpr(sizeOfExpr, sizeVar);
    }
    else if (returnByReference || (!tgt && !result))
    {
        ctx.addExpr(call);

        if (translateSetReturn && tgt)
        {
            CHqlBoundTarget targetLength;
            CHqlBoundExpr boundLength;
            boundLength.expr.setown(getBoundLength(localBound));
            targetLength.expr.set(tgt->length);
            assign(ctx, targetLength, boundLength);
        }
    }
    else
        localBound.expr.set(call);

    if (localBound.expr)
        localBound.expr.setown(convertWrapperToPointer(localBound.expr));
    if (tgt && !doneAssign)
        assign(ctx, *tgt, localBound);
    else if (result)
        result->set(localBound);

    //Old style row target where the row is passed in as a parameter
    if (resultRow)
        finalizeTempRow(ctx, resultRow, resultRowBuilder);

    if (returnByReference)
        ctx.associateExpr(expr, localBound);
}

void HqlCppTranslator::doBuildExprCall(BuildCtx & ctx, IHqlExpression * expr, CHqlBoundExpr & tgt)
{
    doBuildCall(ctx, NULL, expr, &tgt);
}


void HqlCppTranslator::doBuildAssignCall(BuildCtx & ctx, const CHqlBoundTarget & target, IHqlExpression * expr)
{
    ITypeInfo * exprType = expr->queryType()->queryPromotedType();
    if ((exprType->getSize() == UNKNOWN_LENGTH) && target.isFixedSize())
    {
        doBuildExprAssign(ctx, target, expr);
        return;
    }
    ITypeInfo * targetType = target.queryType()->queryPromotedType();
    if ((isStringType(exprType) || isUnicodeType(exprType) || isStringType(targetType) || isUnicodeType(targetType)) && exprType->getTypeCode() != targetType->getTypeCode())
    {
        doBuildExprAssign(ctx, target, expr);
        return;
    }
    if ((exprType->getTypeCode() == type_set) && (queryUnqualifiedType(targetType) != queryUnqualifiedType(exprType)))
    {
        if (exprType->queryChildType()) // allow direct assignment of generic set functions. (e.g., internal)
        {
            doBuildExprAssign(ctx, target, expr);
            return;
        }
    }
    doBuildCall(ctx, &target, expr, NULL);
}

void HqlCppTranslator::doBuildStmtCall(BuildCtx & ctx, IHqlExpression * expr)
{
    doBuildCall(ctx, NULL, expr, NULL);
}

//---------------------------------------------------------------------------

void HqlCppTranslator::doBuildXmlEncode(BuildCtx & ctx, const CHqlBoundTarget * tgt, IHqlExpression * expr, CHqlBoundExpr * result)
{
    node_operator op = expr->getOperator();
    bool isUnicode = isUnicodeType(expr->queryType());
    HqlExprArray args;
    _ATOM func;

    args.append(*LINK(expr->queryChild(0)));
    if (op == no_xmldecode)
        func = isUnicode ? xmlDecodeUStrAtom : xmlDecodeStrAtom;
    else
    {
        func = isUnicode ? xmlEncodeUStrAtom : xmlEncodeStrAtom;
        __int64 flags = 0;
        if (expr->hasProperty(allAtom))
            flags = ENCODE_WHITESPACE;
        args.append(*createConstant(flags));
    }

    OwnedHqlExpr call = bindFunctionCall(func, args);
    if (tgt)
        buildExprAssign(ctx, *tgt, call);
    else
        buildExpr(ctx, call, *result);
}


//---------------------------------------------------------------------------
//-- no_cast --
//-- no_implicitcast --

IValue * getCastValue(ITypeInfo * cast, IHqlExpression * arg)
{
    Owned<IValue> value;
    switch (arg->getOperator())
    {
    case no_constant:
        value.set(arg->queryValue());
        break;
    case no_cast: case no_implicitcast:
        value.setown(getCastValue(arg->queryType(), arg->queryChild(0)));
        break;
    default:
        return NULL;
    }

    if (!value)
        return NULL;
    return value->castTo(cast);
}

inline IHqlExpression * getCastExpr(ITypeInfo * cast, IHqlExpression * arg)
{
    IValue * value = getCastValue(cast, arg);
    if (value)
        return createConstant(value);
    return NULL;
}

void HqlCppTranslator::doBuildAssignCast(BuildCtx & ctx, const CHqlBoundTarget & target, IHqlExpression * expr)
{
    IHqlExpression * left = expr->queryChild(0);

    if (options.foldConstantCast)
    {
        //remove when we have constant folding installed...
        OwnedHqlExpr cast = getCastExpr(expr->queryType(), left);
        if (cast)
        {
            buildExprAssign(ctx, target, cast);
            return;
        }
    }

#if 0
    StringBuffer s;
    s.append("assign cast from=");
    left->queryType()->getECLType(s);
    target.queryType()->getECLType(s.append(" target="));
    expr->queryType()->getECLType(s.append(" expr="));
    PrintLog(s.str());
#endif

    ITypeInfo * targetType = target.queryType();
    ITypeInfo * exprType = expr->queryType();
    if ((targetType->queryPromotedType() == exprType->queryPromotedType()))
    {
        CHqlBoundExpr bound;
        if (ctx.getMatchExpr(left, bound))
            assignAndCast(ctx, target, bound);
        else
        {
            OwnedHqlExpr values = normalizeListCasts(expr);
            if (values != expr)
            {
                buildExprAssign(ctx, target, values);
                return;
            }
            
            bool useTemp = requiresTemp(ctx, left, false) && (left->getOperator() != no_concat) && (left->getOperator() != no_createset);
            if (useTemp && isStringType(targetType) && (left->getOperator() == no_substring) && target.isFixedSize())
            {
                //don't do this if the target type is unicode at the moment
                Owned<ITypeInfo> stretchedType = getStretchedType(targetType->getStringLen(), exprType);
                if (isSameBasicType(stretchedType, targetType->queryPromotedType()))
                    useTemp = false;
            }

            if (useTemp)
            {
                buildExpr(ctx, left, bound);
                assignAndCast(ctx, target, bound);
            }
            else
            {
                buildExprAssign(ctx, target, left);
            }
        }
        return;
    }

    ITypeInfo * leftType = left->queryType();
    if ((targetType->queryPromotedType() == left->queryType()->queryPromotedType()))
    {
        if (preservesValue(exprType, leftType))
        {
            buildExprAssign(ctx, target, left);
            return;
        }
    }

    CHqlBoundExpr pure;
    bool assignDirect = false;
    if ((exprType->getSize() == UNKNOWN_LENGTH) && (targetType->getTypeCode() == exprType->getTypeCode()) &&
        (isStringType(exprType) || isUnicodeType(exprType)))
    {
        OwnedITypeInfo stretched = getStretchedType(UNKNOWN_LENGTH, targetType->queryPromotedType());
        if (stretched == exprType->queryPromotedType())
            assignDirect = true;
    }

    if (assignDirect)
        buildExpr(ctx, left, pure);
    else
        buildExpr(ctx, expr, pure);

    assignAndCast(ctx, target, pure);
}

void HqlCppTranslator::doBuildExprCast(BuildCtx & ctx, IHqlExpression * expr, CHqlBoundExpr & tgt)
{
    IHqlExpression * arg = expr->queryChild(0);
    ITypeInfo * exprType = expr->queryType();

    if (exprType->getTypeCode() == type_set)
    {
        OwnedHqlExpr castArg = ensureExprType(arg, exprType);
        if ((castArg->getOperator() != no_cast) && (castArg->getOperator() != no_implicitcast))
        {
            buildExpr(ctx, castArg, tgt);
            return;
        }
        
        //The case is almost certainly going to go via a temporary anyway, better code is generated if we can avoid an extra assign
        buildTempExpr(ctx, expr, tgt);
        return;
    }

    if (options.foldConstantCast)
    {
        //remove when we have constant folding installed...
        IHqlExpression * cast = getCastExpr(exprType, arg);
        if (cast)
        {
            tgt.expr.setown(cast);
            return;
        }
    }

    // weird special case....  (int4)(swapint4)int4_x  - remove both casts...
    switch (arg->getOperator())
    {
    case no_cast:
    case no_implicitcast:
        {
            IHqlExpression * child = arg->queryChild(0);
            if (child->queryType() == exprType)
            {
                if ((exprType->getTypeCode() == type_int) || (exprType->getTypeCode() == type_swapint) || (exprType->getTypeCode() == type_packedint))
                {
                    ITypeInfo * argType = arg->queryType();
                    if ((argType->getTypeCode() == type_int) || (argType->getTypeCode() == type_swapint) || (argType->getTypeCode() == type_packedint))
                    {
                        if (argType->getSize() == exprType->getSize())
                        {
                            buildExpr(ctx, child, tgt);
                            return;
                        }
                    }
                }
            }
            break;
        }
#if 0
    case no_if:
        {
            //optimize cast of an if where one of the arguments is already the correct type
            //this doesn't really improve things
            IHqlExpression * lhs = arg->queryChild(1);
            IHqlExpression * rhs = arg->queryChild(2);
            if (lhs->queryType() == exprType || lhs->getOperator() == no_constant || 
                rhs->queryType() == exprType || rhs->getOperator() == no_constant)
            {
                HqlExprArray args;
                args.append(*LINK(arg->queryChild(0)));
                args.append(*ensureExprType(lhs, exprType));
                args.append(*ensureExprType(rhs, exprType));
                unwindChildren(args, arg, 3);
                OwnedHqlExpr next = createValue(no_if, LINK(exprType), args);
                buildExpr(ctx, next, tgt);
                return;
            }
            break;
        }
#endif
    case no_substring:
        {
            ITypeInfo * argType = arg->queryType();
            if ((exprType->getSize() != UNKNOWN_LENGTH) && (argType->getSize() == UNKNOWN_LENGTH) && (exprType->getTypeCode() == argType->getTypeCode()))
            {
                OwnedITypeInfo stretched = getStretchedType(exprType->getStringLen(), argType);
                if (stretched == exprType)
                {
                    buildTempExpr(ctx, expr, tgt);
                    return;
                }
            }
            break;
        }
    }

    CHqlBoundExpr pure;
    buildExpr(ctx, arg, pure);

    doBuildExprCast(ctx, exprType, pure, tgt);

    if ((arg->queryType()->getTypeCode() == type_decimal) && !isTypePassedByAddress(exprType))
    {
        OwnedHqlExpr translated = tgt.getTranslatedExpr();
        buildTempExpr(ctx, translated, tgt);
    }
}


void HqlCppTranslator::doBuildCastViaTemp(BuildCtx & ctx, ITypeInfo * to, CHqlBoundExpr & pure, CHqlBoundExpr & tgt)
{
    CHqlBoundTarget boundTarget;
    Linked<ITypeInfo> targetType = to;

    //If the temporary size can be deduced, then use a fixed length temporary to save a heap operation.
    ITypeInfo * fromType = pure.expr->queryType();
    if (isStringType(to) && to->getSize() == UNKNOWN_LENGTH && isStringType(fromType) && !pure.length)
    {
        assertex(fromType->getSize() != UNKNOWN_LENGTH);
        targetType.setown(getStretchedType(fromType->getStringLen(), to));
    }

    OwnedHqlExpr translated = pure.getTranslatedExpr();
    OwnedHqlExpr cast = ensureExprType(translated, targetType);
    buildTempExpr(ctx, cast, tgt);
}

void HqlCppTranslator::doBuildCastViaString(BuildCtx & ctx, ITypeInfo * to, const CHqlBoundExpr & pure, CHqlBoundExpr & tgt)
{
    ITypeInfo * from = pure.expr->queryType();
    Owned<ITypeInfo> tempType = makeStringType(from->getStringLen(), NULL, NULL);
    OwnedHqlExpr temp = createValue(no_implicitcast, LINK(to),
                                    createValue(no_implicitcast, tempType.getLink(), pure.getTranslatedExpr()));
    buildExpr(ctx, temp, tgt);
}

void HqlCppTranslator::doBuildExprCast(BuildCtx & ctx, ITypeInfo * to, CHqlBoundExpr & pure, CHqlBoundExpr & tgt)
{
    ITypeInfo * from = pure.expr->queryType();

    HqlExprArray args;
    _ATOM funcName = NULL;
    OwnedHqlExpr op;
    switch (to->getTypeCode())
    {
        case type_boolean:
            {
                switch (from->getTypeCode())
                {
                case type_string:
                    funcName = an2bAtom;
                    args.append(*getBoundLength(pure));
                    args.append(*getElementPointer(pure.expr));
                    break;
                case type_data:
                    funcName = data2BoolAtom;
                    args.append(*getBoundLength(pure));
                    args.append(*getElementPointer(pure.expr));
                    break;
                case type_qstring:
                    funcName = qstr2BoolAtom;
                    args.append(*getBoundLength(pure));
                    args.append(*getElementPointer(pure.expr));
                    break;
                case type_varstring:
                    funcName = vn2bAtom;
                    args.append(*getElementPointer(pure.expr));
                    break;
                case type_decimal:
                    ensurePushed(ctx, pure);
                    funcName = DecCompareNullAtom;
                    break;
                case type_unicode:
                case type_varunicode:
                case type_utf8:
                    doBuildCastViaString(ctx, to, pure, tgt);
                    return;
                case type_real:
                default:
                    //default action
                    break;
                }
                break;
            }
        case type_packedint:
            {
                ITypeInfo * logicalType = to->queryPromotedType();
                size32_t toSize = logicalType->getSize();
                if ((from->getTypeCode() != type_int) || (toSize != from->getSize()))
                {
                    OwnedHqlExpr translated = pure.getTranslatedExpr();
                    OwnedHqlExpr castTranslated = createValue(no_implicitcast, LINK(logicalType), LINK(translated));
                    buildExpr(ctx, castTranslated, tgt);
                    return;
                }
                tgt.set(pure);
                return;
            }
        case type_swapint:
            {
                if ((from->getTypeCode() == type_swapint) && (to->getSize() == from->getSize()))
                {
                    break;  // default behaviour
                    //MORE: Could special case cast between diff size swapints, but a bit too complicated.
                }
                if (from->getTypeCode() != type_int)
                {
                    ITypeInfo * tempType = makeIntType(to->getSize(), to->isSigned());
                    IHqlExpression * translated = pure.getTranslatedExpr();
                    translated = createValue(no_implicitcast, tempType, translated);
                    OwnedHqlExpr castTranslated = createValue(no_implicitcast, LINK(to), translated);
                    buildExpr(ctx, castTranslated, tgt);
                    return;
                }
                
                unsigned toSize = to->getSize();
                unsigned fromSize = from->getSize();
                if ((toSize == 1) && (fromSize == 1))
                {
                    if (to->isSigned() != from->isSigned())
                        break;
                    tgt.expr.setown(createValue(no_typetransfer, LINK(to), LINK(pure.expr)));
                    return;
                }

                if (toSize != fromSize)
                {
                    Owned<ITypeInfo> tempType = makeIntType(toSize, from->isSigned());

                    CHqlBoundExpr tempInt;
                    tempInt.expr.setown(ensureExprType(pure.expr, tempType));

                    doBuildCastViaTemp(ctx, to, tempInt, tgt);
                }
                else
                    doBuildCastViaTemp(ctx, to, pure, tgt);
                return;
            }
        case type_int:
            {
                switch (from->getTypeCode())
                {
                case type_qstring:
                    {
                        //Need to go via a temporary string.
                        Owned<ITypeInfo> tempType = makeStringType(from->getStringLen(), NULL, NULL);
                        OwnedHqlExpr temp = createValue(no_implicitcast, LINK(to),
                                                        createValue(no_implicitcast, tempType.getLink(), pure.getTranslatedExpr()));
                        buildExpr(ctx, temp, tgt);
                        return;
                    }
                case type_string:
                case type_data:
                    {
                        _ATOM charset = from->queryCharset()->queryName();
                        if (charset == ebcdicAtom)
                        {
                            if (to->isSigned())
                                funcName = (to->getSize() > 4 ? en2ls8Atom : en2ls4Atom);
                            else
                                funcName = (to->getSize() > 4 ? en2l8Atom : en2l4Atom);
                        }
                        else if ((charset == asciiAtom) || (charset == dataAtom))
                        {
                            if (to->isSigned())
                                funcName = (to->getSize() > 4 ? an2ls8Atom : an2ls4Atom);
                            else
                                funcName = (to->getSize() > 4 ? an2l8Atom : an2l4Atom);
                        }
                        else
                            assertex(!"Unknown character set");
                        //MORE: This should really cast the result to the real width to remove extra bytes.
                        //e.g. (unsigned3)-1 should be 0xffffff, not 0xffffffff

                        args.append(*getBoundLength(pure));
                        args.append(*getElementPointer(pure.expr));
                        break;
                    }
                case type_varstring:
                    if (to->isSigned())
                        funcName = (to->getSize() > 4 ? vn2ls8Atom : vn2ls4Atom);
                    else
                        funcName = (to->getSize() > 4 ? vn2l8Atom : vn2l4Atom);
                    args.append(*getElementPointer(pure.expr));
                    break;
                case type_decimal:
                    ensurePushed(ctx, pure);
                    if (to->getSize() > 4)
                        funcName = DecPopInt64Atom;
                    else
                        funcName = DecPopLongAtom;
                    break;
                case type_packedint:
                    {
                        if (to->getSize() < from->getSize())
                        {
                            funcName = castIntAtom[to->getSize()][to->isSigned()];
                            args.append(*LINK(pure.expr));
                        }
                        else
                            tgt.set(pure);
                        break;
                    }
                case type_swapint:
                    {
                        unsigned toSize = to->getSize();
                        unsigned fromSize = from->getSize();
                        if ((toSize == 1) && (fromSize == 1))
                        {
                            if (to->isSigned() != from->isSigned())
                                break;
                            tgt.expr.setown(createValue(no_typetransfer, LINK(to), LINK(pure.expr)));
                            return;
                        }

                        if (toSize != fromSize)
                        {
                            Owned<ITypeInfo> tempType = makeIntType(fromSize, from->isSigned());
                            CHqlBoundExpr tempInt;
                            doBuildCastViaTemp(ctx, tempType, pure, tempInt);
                            funcName = castIntAtom[to->getSize()][to->isSigned()];
                            if (funcName && toSize < fromSize)
                            {
                                args.append(*LINK(tempInt.expr));
                                tgt.expr.setown(bindTranslatedFunctionCall(funcName, args));
                            }
                            else
                                tgt.expr.setown(ensureExprType(tempInt.expr, to));
                        }
                        else
                            doBuildCastViaTemp(ctx, to, pure, tgt);
                        return;
                    }
                case type_int:
                    if (to->getSize() < from->getSize())
                    {
                        _ATOM name = castIntAtom[to->getSize()][to->isSigned()];
                        if (name)
                        {
                            args.append(*LINK(pure.expr));
                            IHqlExpression * call = bindTranslatedFunctionCall(name, args);
                            op.setown(createValue(no_typetransfer, LINK(to), call));
                        }
                    }
                    break;
                case type_unicode:
                case type_varunicode:
                case type_utf8:
                    doBuildCastViaString(ctx, to, pure, tgt);
                    return;
                case type_real:
                default:
                    //default action
                    break;
                }
                break;
            }
        case type_real:
            switch (from->getTypeCode())
            {
                case type_qstring:
                case type_unicode:
                case type_varunicode:
                case type_utf8:
                    doBuildCastViaString(ctx, to, pure, tgt);
                    return;
                case type_data:
                case type_string:
                    funcName = from->queryCharset()->queryName() != ebcdicAtom ?  an2fAtom : en2fAtom;
                    args.append(*getBoundLength(pure));
                    args.append(*getElementPointer(pure.expr));
                    break;
                case type_varstring:
                    funcName = from->queryCharset()->queryName() != ebcdicAtom ?  vn2fAtom : ex2fAtom;
                    args.append(*getElementPointer(pure.expr));
                    break;
                case type_decimal:
                    ensurePushed(ctx, pure);
                    funcName = DecPopRealAtom;
                    break;
                case type_swapint:
                    {
                        //cast via intermediate int.
                        ITypeInfo * type = makeIntType(from->getSize(), from->isSigned());
                        IHqlExpression * translated = pure.getTranslatedExpr();
                        OwnedHqlExpr castTranslated = createValue(no_implicitcast, type, translated);
                        pure.clear();
                        buildExpr(ctx, castTranslated, pure);
                        from = type;
                    }
                    //fallthrough
                case type_int:
                case type_boolean:
                case type_packedint:
                default:
                    //default action
                    break;
            }
            break;
        case type_decimal:
            {
                ensurePushed(ctx, pure);

                bool needToSetPrecision = true;
                switch (from->getTypeCode())
                {
                case type_int:
                case type_swapint:
                    if (to->getDigits() >= from->getDigits())
                        needToSetPrecision = false;
                    break;
                case type_decimal:
                    if (((to->getDigits() - to->getPrecision()) >= (from->getDigits() - from->getPrecision())) &&
                        (to->getPrecision() >= from->getPrecision()))
                        needToSetPrecision = false;
                    break;
                }

                if (needToSetPrecision)
                {
                    args.append(*createConstant(createIntValue(to->getDigits(), 1, false)));
                    args.append(*createConstant(createIntValue(to->getPrecision(), 1, false)));
                    callProcedure(ctx, DecSetPrecisionAtom, args);
                }

                op.setown(createValue(no_decimalstack, LINK(to)));
            }
            break;
        case type_set:
            {
                //MORE: Shouldn't have to create this node...
                OwnedHqlExpr cast = createValue(no_implicitcast, LINK(to), pure.getTranslatedExpr());
                buildTempExpr(ctx, cast, tgt);
                return;
            }
        case type_pointer:
        case type_row:
            break;
        case type_varstring:
            if ((to->getSize() == UNKNOWN_LENGTH) && (from->getTypeCode() == type_varstring))
                tgt.set(pure);
            else
                doBuildCastViaTemp(ctx, to, pure, tgt);
            return;
        case type_string:
        case type_data:
            {
                if (canRemoveStringCast(to, from))
                {
                    ICharsetInfo * srcset = from->queryCharset();
                    ICharsetInfo * tgtset = to->queryCharset();
                    
                    //Data never calls a conversion function... but does add a cast
                    if ((srcset == tgtset) || (to->getTypeCode() == type_data) || (from->getTypeCode() == type_data))
                    {
                        if (from->getTypeCode() == type_varstring)
                            tgt.length.setown(getBoundLength(pure));
                        else
                            tgt.length.set(pure.length);

                        Owned<ITypeInfo> newType;
                        if (to->getSize() == UNKNOWN_LENGTH)
                            newType.setown(getStretchedType(from->getSize(), to));
                        else
                            newType.set(to);

                        if (from->getTypeCode() != type_data)
                        {
                            newType.setown(cloneModifiers(from, newType));

                            tgt.expr.setown(createValue(no_typetransfer, newType.getClear(), pure.expr.getLink()));
                        }
                        else
                        {
                            IHqlExpression * base = queryStripCasts(pure.expr);
                            newType.setown(makeReferenceModifier(newType.getClear()));
                            tgt.expr.setown(createValue(no_cast, newType.getClear(), LINK(base)));
                        }
                        return;
                    }
                }
            }
            doBuildCastViaTemp(ctx, to, pure, tgt);
            return;
        case type_unicode:
        case type_varunicode:
            if ((from->getTypeCode() == to->getTypeCode()) && (to->getSize() == UNKNOWN_LENGTH))
            {
                tgt.set(pure);
                return;
            }
            doBuildCastViaTemp(ctx, to, pure, tgt);
            return;
        default:
            doBuildCastViaTemp(ctx, to, pure, tgt);
            return;
    }

    if (funcName)
        op.setown(bindTranslatedFunctionCall(funcName, args));
    if (!op)
        op.setown(ensureExprType(pure.expr, to));

    if (queryUnqualifiedType(op->queryType()) != queryUnqualifiedType(to))
    {
        OwnedHqlExpr translated = createTranslated(op);
        OwnedHqlExpr cast = ensureExprType(translated, to);
        buildExpr(ctx, cast, tgt);
    }
    else
        tgt.expr.setown(op.getClear());
}

//---------------------------------------------------------------------------
//-- no_char_length --

//NB: parameter is expression to take length of - not the length node.

IHqlExpression * HqlCppTranslator::doBuildCharLength(BuildCtx & ctx, IHqlExpression * expr)
{
    CHqlBoundExpr bound;
    buildCachedExpr(ctx, expr, bound);
    return getBoundLength(bound);
}

//---------------------------------------------------------------------------
//-- no_choose

void HqlCppTranslator::doBuildChoose(BuildCtx & ctx, const CHqlBoundTarget * target, IHqlExpression * expr)
{
    CHqlBoundExpr test;
    
    BuildCtx subctx(ctx);
    buildExpr(subctx, expr->queryChild(0), test);
    IHqlStmt * stmt = subctx.addSwitch(test.expr);
    
    unsigned max = expr->numChildren()-1;
    unsigned idx;
    for (idx = 1; idx < max; idx++)
    {
        OwnedHqlExpr branch = getSizetConstant(idx);
        subctx.addCase(stmt, branch);
        buildExprOrAssign(subctx, target, expr->queryChild(idx), NULL);
    }

    subctx.addDefault(stmt);
    buildExprOrAssign(subctx, target, expr->queryChild(max), NULL);
}

void HqlCppTranslator::doBuildAssignChoose(BuildCtx & ctx, const CHqlBoundTarget & target, IHqlExpression * expr)
{
    unsigned max = expr->numChildren()-1;
    unsigned idx;
    bool allConstant = true;
    for (idx = 1; idx < max; idx++)
    {
        if (!expr->queryChild(idx)->queryValue())
            allConstant = false;
    }

    if (allConstant)
    {
        //MORE: Need to calculate the correct type.
        HqlExprArray args;
        args.append(*LINK(expr->queryChild(0)));
        for (idx = 1; idx < max; idx++)
        {
            IHqlExpression * v1 = createConstant(createIntValue(idx, LINK(unsignedType)));
            IHqlExpression * v2 = expr->queryChild(idx);
            ITypeInfo * type = v2->queryType();
            args.append(* createValue(no_mapto, LINK(type), v1, LINK(v2)));
        }
        args.append(*LINK(expr->queryChild(max)));

        OwnedHqlExpr caseExpr = createValue(no_case, expr->getType(), args);
        buildExprAssign(ctx, target, caseExpr);
    }
    else
    {
        doBuildChoose(ctx, &target, expr);
    }
}



//---------------------------------------------------------------------------
//-- compare (no_eq,no_ne,no_lt,no_gt,no_le,no_ge) --

//Are the arguments scalar?  If so return the scalar arguments.
static bool getIsScalarCompare(IHqlExpression * expr, OwnedHqlExpr & left, OwnedHqlExpr & right)
{
    left.set(expr->queryChild(0));
    right.set(expr->queryChild(1));

    ITypeInfo * lType = left->queryType();
    ITypeInfo * rType = right->queryType();
    type_t leftTypeCode = lType ? lType->getTypeCode() : type_row;
    type_t rightTypeCode = rType ? rType->getTypeCode() : type_row;
    assertex(leftTypeCode == rightTypeCode);

    switch (leftTypeCode)
    {
    case type_row:
    case type_set:
        return false;
    default:
        return true;
    }
}


void HqlCppTranslator::doBuildAssignCompare(BuildCtx & ctx, const CHqlBoundTarget & target, IHqlExpression * expr)
{
    OwnedHqlExpr left;
    OwnedHqlExpr right;
    if (getIsScalarCompare(expr, left, right))
    {
        doBuildExprAssign(ctx, target, expr);
        return;
    }

    //Comparing a row - calculate the ordering and then compare against zero...
    IHqlExpression * compare = createValue(no_order, LINK(signedType), LINK(left), LINK(right));
    OwnedHqlExpr temp = createBoolExpr(expr->getOperator(), compare, getZero());
    buildExprAssign(ctx, target, temp);
}


//---------------------------------------------------------------------------
//-- no_concat --

void HqlCppTranslator::buildConcatFArgs(HqlExprArray & args, BuildCtx & ctx, const HqlExprArray & values, ITypeInfo * argType)
{
    ForEachItemIn(idx, values)
    {
        IHqlExpression * cur = &values.item(idx);
        OwnedHqlExpr value = getCastParameter(cur, argType);
        CHqlBoundExpr bound;
        buildCachedExpr(ctx, value, bound);

        //The function takes ... so the length must be 4 bytes or the stack
        //gets out of sync...
        OwnedHqlExpr length = getBoundLength(bound);
        args.append(*ensureExprType(length, unsignedType));
        args.append(*getElementPointer(bound.expr));
    }
    args.append(*createConstant(signedType->castFrom(true, -1))); // correct size terminator
}

void HqlCppTranslator::doBuildVarLengthConcatF(BuildCtx & ctx, const CHqlBoundTarget & target, const HqlExprArray & values)
{
    ITypeInfo * targetType = target.queryType();
    Linked<ITypeInfo> argType = targetType;
    type_t ttc = targetType->getTypeCode();

    HqlExprArray args;
    _ATOM func;

    if (ttc == type_varstring)
    {
        func = concatVStrAtom;
        argType.setown(makeStringType(UNKNOWN_LENGTH, LINK(targetType->queryCharset()), LINK(targetType->queryCollation())));
    }
    else if (ttc == type_unicode)
    {
        args.append(*target.length.getLink());
        func = concatUnicodeAtom;
    }
    else if (ttc == type_utf8)
    {
        args.append(*target.length.getLink());
        func = concatUtf8Atom;
    }
    else if (ttc == type_varunicode)
    {
        func = concatVUnicodeAtom;
        argType.setown(makeUnicodeType(UNKNOWN_LENGTH, targetType->queryLocale()));
    }
    else
    {
        args.append(*target.length.getLink());
        func = concatAtom;
    }

    IHqlExpression * tgt = createValue(no_address, makeVoidType(), LINK(target.expr));
    if (ttc == type_data)
        tgt = createValue(no_implicitcast, makePointerType(makeStringType(UNKNOWN_LENGTH, NULL, NULL)), tgt);
    args.append(*tgt);
    buildConcatFArgs(args, ctx, values, argType);
    callProcedure(ctx, func, args);
}

bool HqlCppTranslator::doBuildFixedLengthConcatF(BuildCtx & ctx, const CHqlBoundTarget & target, const HqlExprArray & values)
{
    ITypeInfo * targetType = target.queryType();
    Owned<ITypeInfo> argType = getStretchedType(UNKNOWN_LENGTH, targetType);
    type_t ttc = targetType->getTypeCode();

    HqlExprArray args;
    _ATOM func = NULL;
    OwnedHqlExpr fill;

    switch (ttc)
    {
    case type_varstring:
        func = concatVStrFAtom;
        argType.setown(makeStringType(UNKNOWN_LENGTH, LINK(targetType->queryCharset()), LINK(targetType->queryCollation())));
        break;
    case type_varunicode:
        func = concatVUnicodeFAtom;
        argType.setown(makeUnicodeType(UNKNOWN_LENGTH, targetType->queryLocale()));
        break;
    case type_unicode:
        func = concatUnicodeFAtom;
        break;
    case type_string:
        func = concatStrFAtom;
        if (targetType->queryCharset()->queryName() == ebcdicAtom)
            fill.setown(getSizetConstant('@'));
        else
            fill.setown(getSizetConstant(' '));
        break;
    case type_data:
        func = concatStrFAtom;
        fill.setown(getSizetConstant(0));
        break;
    }

    if (func)
    {
        args.append(*getSizetConstant(targetType->getStringLen()));
        args.append(*getPointer(target.expr));
        if (fill)
            args.append(*LINK(fill));
        buildConcatFArgs(args, ctx, values, argType);
        callProcedure(ctx, func, args);
        return true;
    }
    return false;
}

void HqlCppTranslator::doBuildAssignConcat(BuildCtx & ctx, const CHqlBoundTarget & target, IHqlExpression * expr)
{
    ITypeInfo * targetType = target.queryType();
    ICharsetInfo * targetCharset = targetType->queryCharset();
    type_t ttc = targetType->getTypeCode();
    switch (ttc)
    {
    case type_unicode:
    case type_varunicode:
    case type_utf8:
        break;
    case type_string:
    case type_varstring:
    case type_data:
        if (!queryDefaultTranslation(targetCharset, expr->queryType()->queryCharset()))
            break;
        //fallthrough
    default:
        doBuildExprAssign(ctx, target, expr);
        return;
    }

    HqlExprArray values;
    expr->unwindList(values, no_concat);

    //Combine adjacent constants (not folded previously because of the tree structure)
    //Not optimized in the folder since it can mess up cse evaluation
    for (unsigned i=0; i < values.ordinality()-1; i++)
    {
        IValue * firstValue = values.item(i).queryValue();
        if (firstValue)
        {
            Linked<IValue> combinedValue = firstValue;
            while (i+1 < values.ordinality())
            {
                IValue * nextValue = values.item(i+1).queryValue();
                if (!nextValue)
                    break;
                combinedValue.setown(concatValues(combinedValue, nextValue));
                values.remove(i+1);
            }
            if (combinedValue->queryType()->getStringLen() == 0)
            {
                values.remove(i);
                i--;        // not nice, but will be incremented before comparison so safe
            }
            else if (combinedValue != firstValue)
                values.replace(*createConstant(combinedValue.getClear()), i);
        }
    }

    if (!target.isFixedSize())
    {
        doBuildVarLengthConcatF(ctx, target, values);
    }
    else if (!doBuildFixedLengthConcatF(ctx, target, values))
    {
        Owned<ITypeInfo> varType = getStretchedType(UNKNOWN_LENGTH, targetType);
        OwnedHqlExpr castValue = createValue(no_concat, LINK(varType), values);
        doBuildExprAssign(ctx, target, castValue);
    }
}

//---------------------------------------------------------------------------
//-- no_div --
// also used for no_modulus

void HqlCppTranslator::doBuildExprDivide(BuildCtx & ctx, IHqlExpression * expr, CHqlBoundExpr & tgt)
{
    if (expr->queryType()->getTypeCode() == type_decimal)
    {
        doBuildExprArith(ctx, expr, tgt);
        return;
    }
    IHqlExpression * left = expr->queryChild(0);
    IHqlExpression * right = expr->queryChild(1);
    assertex(left->queryType() == right->queryType());

    IValue * value = right->queryValue();
    if (value)
    {
        Owned<IValue> zero = right->queryType()->castFrom(false, 0);
        int cmp = value->compare(zero);

        if (cmp == 0)
            tgt.expr.setown(createConstant(zero.getClear()));
        else
            doBuildPureSubExpr(ctx, expr, tgt);
    }
    else
    {
        buildTempExpr(ctx, expr, tgt);
    }
}


void HqlCppTranslator::doBuildAssignDivide(BuildCtx & ctx, const CHqlBoundTarget & target, IHqlExpression * expr)
{
    if (expr->queryType()->getTypeCode() == type_decimal)
    {
        doBuildExprAssign(ctx, target, expr);
        return;
    }
    IHqlExpression * left = expr->queryChild(0);
    IHqlExpression * right = expr->queryChild(1);
    assertex(left->queryType() == right->queryType());

    CHqlBoundExpr lhs,rhs;
    buildExpr(ctx, left, lhs);
    buildSimpleExpr(ctx, right, rhs);

    IHqlExpression * divisor = rhs.expr.get();
    OwnedHqlExpr pureExpr = createValue(expr->getOperator(), left->getType(), lhs.expr.getClear(), LINK(divisor));
    IValue * zero = pureExpr->queryType()->castFrom(false, 0);
    OwnedHqlExpr eZero = createConstant(zero);

    IValue * value = rhs.expr->queryValue();
    if (value)
    {
        int cmp = value->compare(eZero->queryValue());

        if (cmp == 0)
            assignBound(ctx, target, eZero);
        else
            assignBound(ctx, target, pureExpr);
    }
    else
    {
        BuildCtx subctx(ctx);
        IHqlStmt * stmt = subctx.addFilter(divisor);
        assignBound(subctx, target, pureExpr);
        subctx.selectElse(stmt);
        assignBound(subctx, target, eZero);
    }
}


//---------------------------------------------------------------------------
//-- no_if --

bool HqlCppTranslator::ifRequiresAssignment(BuildCtx & ctx, IHqlExpression * expr)
{
    IHqlExpression * trueExpr = expr->queryChild(1);
    IHqlExpression * falseExpr = expr->queryChild(2);

    if (requiresTemp(ctx, trueExpr, true) || requiresTemp(ctx, falseExpr, true) || expr->queryType()->getSize() == UNKNOWN_LENGTH)
        return true;
    if (trueExpr->queryType() != falseExpr->queryType() && isStringType(expr->queryType()))
        return true;
    if (expr->queryType()->getTypeCode() == type_decimal)
        return true;
    return false;
}

void HqlCppTranslator::doBuildAssignIf(BuildCtx & ctx, const CHqlBoundTarget & target, IHqlExpression * expr)
{
    if (!ifRequiresAssignment(ctx, expr))
    {
        doBuildExprAssign(ctx, target, expr);
        return;
    }

    IHqlExpression * trueExpr = expr->queryChild(1);
    IHqlExpression * falseExpr = expr->queryChild(2);

    BuildCtx subctx(ctx);
    CHqlBoundExpr cond;
    buildCachedExpr(subctx, expr->queryChild(0), cond);

    IHqlStmt * test = subctx.addFilter(cond.expr);
    buildExprAssign(subctx, target, trueExpr);

    subctx.selectElse(test);
    buildExprAssign(subctx, target, falseExpr);
}

void HqlCppTranslator::doBuildExprIf(BuildCtx & ctx, IHqlExpression * expr, CHqlBoundExpr & tgt)
{
    if (ifRequiresAssignment(ctx, expr))
    {
        buildTempExpr(ctx, expr, tgt);
        return;
    }

    IHqlExpression * trueExpr = expr->queryChild(1);
    IHqlExpression * falseExpr = expr->queryChild(2);

    //Length should not be conditional...
    CHqlBoundExpr cond;
    CHqlBoundExpr boundTrue;
    CHqlBoundExpr boundFalse;
    buildCachedExpr(ctx, expr->queryChild(0), cond);
    buildCachedExpr(ctx, trueExpr, boundTrue);
    buildCachedExpr(ctx, falseExpr, boundFalse);
    //true and false should have same type...
    tgt.expr.setown(createValue(no_if, expr->getType(), cond.expr.getClear(), boundTrue.expr.getClear(), boundFalse.expr.getClear()));
}


void HqlCppTranslator::doBuildStmtIf(BuildCtx & ctx, IHqlExpression * expr)
{
    BuildCtx subctx(ctx);
    CHqlBoundExpr cond;
    buildCachedExpr(subctx, expr->queryChild(0), cond);

    IHqlStmt * test = subctx.addFilter(cond.expr);
    buildStmt(subctx, expr->queryChild(1));

    IHqlExpression * elseExpr = queryRealChild(expr, 2);
    if (elseExpr && elseExpr->getOperator() != no_null)
    {
        subctx.selectElse(test);
        buildStmt(subctx, elseExpr);
    }
}

//---------------------------------------------------------------------------
//-- no_intformat --

IHqlExpression * HqlCppTranslator::createFormatCall(_ATOM func, IHqlExpression * expr)
{
    HqlExprArray args;
    unsigned max = expr->numChildren();
    unsigned idx;
    for (idx=0; idx < max; idx++)
    {
        IHqlExpression * cur = expr->queryChild(idx);;
        args.append(*LINK(cur));
    }
    return bindFunctionCall(func, args);
}


void HqlCppTranslator::doBuildExprFormat(_ATOM func, BuildCtx & ctx, IHqlExpression * expr, CHqlBoundExpr & tgt)
{
    OwnedHqlExpr call = createFormatCall(func, expr);
    buildExpr(ctx, call, tgt);
}

void HqlCppTranslator::doBuildAssignFormat(_ATOM func, BuildCtx & ctx, const CHqlBoundTarget & target, IHqlExpression * expr)
{
    OwnedHqlExpr call = createFormatCall(func, expr);
    buildExprAssign(ctx, target, call);
}

//---------------------------------------------------------------------------

void HqlCppTranslator::doBuildAssignToXml(BuildCtx & ctx, const CHqlBoundTarget & target, IHqlExpression * expr)
{
    IHqlExpression * row = expr->queryChild(0);

    HqlExprArray args;
    args.append(*buildMetaParameter(row));
    args.append(*LINK(row));
    args.append(*getSizetConstant(XWFtrim|XWFopt|XWFnoindent));
    OwnedHqlExpr call = bindFunctionCall(ctxGetRowXmlAtom, args);
    buildExprAssign(ctx, target, call);
}

//---------------------------------------------------------------------------

void HqlCppTranslator::doBuildExprCppBody(BuildCtx & ctx, IHqlExpression * expr, CHqlBoundExpr * tgt)
{
    if (!allowEmbeddedCpp())
        throwError(HQLERR_EmbeddedCppNotAllowed);

    StringBuffer text;
    expr->queryChild(0)->queryValue()->getStringValue(text);
    OwnedHqlExpr quoted = createQuoted(text.str(), expr->getType());

    if (tgt)
    {
        ITypeInfo * type = expr->queryType();
        assertex(type->getTypeCode() == type_varstring || type->getSize() != UNKNOWN_LENGTH);
        tgt->expr.set(quoted);
    }
    else
        ctx.addExpr(quoted);
}


//---------------------------------------------------------------------------
//-- no_index --
IHqlExpression * getSimpleListIndex(BuildCtx & ctx, IHqlExpression * expr)
{
    IHqlExpression * index = expr->queryChild(1);
    if (!index->isConstant())
        return NULL;
    OwnedHqlExpr set = normalizeListCasts(expr->queryChild(0));
    switch (set->getOperator())
    {
    case no_null:
    case no_list:
        break;
    default:
        return NULL;
    }
    OwnedHqlExpr folded = foldHqlExpression(index);
    __int64 which = folded->queryValue()->getIntValue();
    if ((which > 0) && (which <= set->numChildren()))
        return LINK(set->queryChild((unsigned)which-1));
    return createNullExpr(expr);
}

void HqlCppTranslator::doBuildExprIndex(BuildCtx & ctx, IHqlExpression * expr, CHqlBoundExpr & tgt)
{
    OwnedHqlExpr simple = getSimpleListIndex(ctx, expr);
    if (simple)
        buildExpr(ctx, simple, tgt);
    else
    {
        OwnedHqlExpr simpleList = simplifyFixedLengthList(expr->queryChild(0));
        Owned<IHqlCppSetCursor> cursor = createSetSelector(ctx, simpleList);
        cursor->buildExprSelect(ctx, expr, tgt);
    }
}

void HqlCppTranslator::doBuildAssignIndex(BuildCtx & ctx, const CHqlBoundTarget & target, IHqlExpression * expr)
{
    OwnedHqlExpr simple = getSimpleListIndex(ctx, expr);
    if (simple)
        buildExprAssign(ctx, target, simple);
    else
    {
        OwnedHqlExpr simpleList = simplifyFixedLengthList(expr->queryChild(0));
        Owned<IHqlCppSetCursor> cursor = createSetSelector(ctx, simpleList);
        cursor->buildAssignSelect(ctx, target, expr);
    }
}

//---------------------------------------------------------------------------
//-- no_list --

bool isComplexSet(ITypeInfo * type)
{
    ITypeInfo * childType = type->queryChildType();
    if (!childType)
        return false;
    switch (childType->getTypeCode())
    {
    case type_alien:
        return true;
    case type_string:
    case type_qstring:
    case type_data:
    case type_unicode:
    case type_varstring:
    case type_varunicode:
        return (childType->getSize() == UNKNOWN_LENGTH);
    case type_utf8:
    case type_swapint:
    case type_packedint:
        return true;
    case type_int:
        switch (childType->getSize())
        {
        case 3: case 5: case 6: case 7:
            return true;
        }
        return false;
    }
    return false;
}

bool isComplexSet(IHqlExpression * expr)
{
    return isComplexSet(expr->queryType());
}

bool isConstantSet(IHqlExpression * expr)
{
    unsigned max = expr->numChildren();
    unsigned idx;
    for (idx = 0; idx < max; idx++)
    {
        IHqlExpression * child = expr->queryChild(idx);
        if (!child->queryValue())
            return false;
    }
    return true;
}


void HqlCppTranslator::doBuildExprConstList(BuildCtx & ctx, IHqlExpression * expr, CHqlBoundExpr & tgt)
{
    BuildCtx declareCtx(*code, literalAtom);

    if (!declareCtx.getMatchExpr(expr, tgt))
    {
        ITypeInfo * type = expr->queryType();
        Linked<ITypeInfo> elementType = type->queryChildType();
        if (!elementType)
            throwError(HQLERR_NullSetCannotGenerate);
        Owned<ITypeInfo> transferType;

        LinkedHqlExpr values = expr;
        if ((isTypePassedByAddress(elementType) && (elementType->getTypeCode() != type_varstring)))
        {
            if (elementType->isReference())
            {
                // use a var string type to get better C++ generated...
                transferType.set(elementType);
                elementType.setown(makeVarStringType(UNKNOWN_LENGTH));
            }
            else
            {
                // for string, data and qstring we need to initialize the array with a list of characters instead of 
                // a cstring e.g., char[][2] = { { 'a','b' }, { 'c', 'd' } };
                HqlExprArray newValues;
                ForEachChild(idx, expr)
                {
                    IHqlExpression * next = expr->queryChild(idx);
                    newValues.append(*createValue(no_create_initializer, next->getType(), LINK(next)));
                }
                values.setown(createValue(no_list, makeSetType(LINK(elementType)), newValues));
            }
        }

        unsigned numElements = expr->numChildren();
        Owned<ITypeInfo> t = makeConstantModifier(makeArrayType(LINK(elementType), numElements));
        IHqlExpression * table = declareCtx.getTempDeclare(t, values);

        if (transferType)
        {
            ITypeInfo * arrayType = makeArrayType(LINK(transferType), numElements);
            table = createValue(no_typetransfer, arrayType, table);
        }

        tgt.count.setown(getSizetConstant(numElements));
        tgt.expr.setown(table);

        //make sure tables get added before any global functions
        declareCtx.associateExpr(expr, tgt);

        if (options.spanMultipleCpp)
        {
            BuildCtx protoctx(*code, mainprototypesAtom);
            protoctx.addDeclareExternal(table);
        }
    }
}

void HqlCppTranslator::doBuildExprDynList(BuildCtx & ctx, IHqlExpression * expr, CHqlBoundExpr & tgt)
{
    if (!ctx.getMatchExpr(expr, tgt))
    {
        ITypeInfo * type = expr->queryType();
        ITypeInfo * elementType = type->queryChildType();

        unsigned max = expr->numChildren();

        //MORE: What if this is an array of variable length strings?
        Owned<ITypeInfo> t = makeArrayType(LINK(elementType), max);
        IHqlExpression * table = ctx.getTempDeclare(t, NULL);

        // new code - should really use a selector...
        unsigned idx;
        CHqlBoundTarget boundTarget;
        for (idx = 0; idx < max; idx++)
        {
            IHqlExpression * child = expr->queryChild(idx);
            boundTarget.expr.setown(createValue(no_index, LINK(elementType), LINK(table), createConstant((int)idx)));
            buildExprAssign(ctx, boundTarget, child);
        }

        tgt.count.setown(getSizetConstant(max));
        tgt.expr.setown(table);
        ctx.associateExpr(expr, tgt);
    }
}

void HqlCppTranslator::doBuildExprList(BuildCtx & ctx, IHqlExpression * _expr, CHqlBoundExpr & tgt)
{
    OwnedHqlExpr expr = simplifyFixedLengthList(_expr);
    ITypeInfo * type = expr->queryType();
    switch (type->getTypeCode())
    {
    case type_set:
    case type_array:
        {
            LinkedHqlExpr values = expr;
            ITypeInfo * childType = type->queryChildType();
            //MORE: Also alien data types and other weird things...
            //if (childType->getSize() == UNKNOWN_LENGTH)
            if (expr->numChildren() == 0)
            {
                tgt.length.setown(getSizetConstant(0));
                tgt.expr.setown(createValue(no_nullptr, makeReferenceModifier(LINK(type))));
                return;
            }
            else if (isComplexSet(expr))
            {
                buildTempExpr(ctx, expr, tgt);
                return;
            }
            else if (childType->getSize() == 0)
            {
                //codes := [''] convert it to a set of a single char string for the moment.
                Owned<ITypeInfo> newType = makeSetType(getStretchedType(1, childType));
                values.setown(ensureExprType(expr, newType));
            }

            if (isConstantSet(expr))
                doBuildExprConstList(ctx, values, tgt);
            else
                doBuildExprDynList(ctx, values, tgt);
            tgt.isAll.set(queryBoolExpr(false));
        }
        break;
    default:
        throw(!"This type of list not supported yet");
    }
}

void HqlCppTranslator::doBuildAssignList(BuildCtx & ctx, const CHqlBoundTarget & target, IHqlExpression * _expr)
{
    OwnedHqlExpr expr = simplifyFixedLengthList(_expr);
    node_operator op = expr->getOperator();
    assertex(op == no_list);

    ITypeInfo * type = expr->queryType();
    switch (type->getTypeCode())
    {
    case type_set:
    case type_array:
        break;
    default:
        throw(!"This type of list not supported yet");
    }

    //This is an assignment, a non-constant set would end up creating two temporaries.
    unsigned numItems = expr->numChildren();
    if (((numItems > 0) && (numItems < 3)) || isComplexSet(expr) || !isConstantSet(expr))
    {
        Owned<IHqlCppSetBuilder> builder = createTempSetBuilder(target.queryType()->queryChildType(), target.isAll);
        builder->buildDeclare(ctx);
        buildSetAssign(ctx, builder, expr);
        builder->buildFinish(ctx, target);
    }
    else if (numItems == 0)
    {
        CHqlBoundExpr temp;
        buildExpr(ctx, expr, temp);
        if (target.isAll)
        {
            if (temp.isAll)
                ctx.addAssign(target.isAll, temp.isAll);
            else
                ctx.addAssign(target.isAll, queryBoolExpr(false));
        }
        ctx.addAssign(target.length, temp.length);
        ctx.addAssign(target.expr, temp.expr);
    }
    else
    {
        OwnedHqlExpr cast = ensureExprType(expr, target.queryType());
        // can do a direct assignment without any casts
        doBuildExprAssign(ctx, target, cast);
    }
}
void HqlCppTranslator::doBuildExprAll(BuildCtx & ctx, IHqlExpression * expr, CHqlBoundExpr & tgt)
{
    tgt.isAll.set(queryBoolExpr(true));
    tgt.length.setown(getSizetConstant(0));
    tgt.expr.setown(createQuoted("0", makeSetType(NULL)));
}

void HqlCppTranslator::doBuildAssignAll(BuildCtx & ctx, const CHqlBoundTarget & target, IHqlExpression * expr)
{
    CHqlBoundExpr temp;
    buildExpr(ctx, expr, temp);
    ctx.addAssign(target.isAll, temp.isAll);
    ctx.addAssign(target.length, temp.length);
    ctx.addAssign(target.expr, temp.expr);
}

//---------------------------------------------------------------------------
//-- no_not --

void HqlCppTranslator::doBuildExprNot(BuildCtx & ctx, IHqlExpression * expr, CHqlBoundExpr & tgt)
{
    ITypeInfo * type = expr->queryChild(0)->queryType();
    switch (type->getTypeCode())
    {
    case type_decimal:
        {
            //MORE: Could leak decimal stack....  Is the last line correct?
            HqlExprArray args;              
            bindAndPush(ctx, expr->queryChild(0));
            IHqlExpression * op = bindTranslatedFunctionCall(DecCompareNullAtom, args);
            tgt.expr.setown(createValue(expr->getOperator(), LINK(boolType), op, getZero()));
        }
        break;
    default:
        doBuildPureSubExpr(ctx, expr, tgt);
        break;
    }
}


//---------------------------------------------------------------------------
//-- no_or --

IHqlExpression * HqlCppTranslator::convertOrToAnd(IHqlExpression * expr)
{
    bool invert = true;
    if (expr->getOperator() == no_not)
    {
        invert = false;
        expr = expr->queryChild(0);
    }
    assertex(expr->getOperator() == no_or);
    HqlExprArray original, inverted;
    expr->unwindList(original, no_or);
    ForEachItemIn(idx, original)
        inverted.append(*getInverse(&original.item(idx)));
    IHqlExpression * ret = createValue(no_and, makeBoolType(), inverted);
    if (invert)
        ret = createValue(no_not, makeBoolType(), ret);
    return ret;
}


//---------------------------------------------------------------------------
// no_unicodeorder

void HqlCppTranslator::doBuildAssignUnicodeOrder(BuildCtx & ctx, const CHqlBoundTarget & target, IHqlExpression * expr)
{
    CHqlBoundExpr lhs, rhs, locale, strength;
    buildCachedExpr(ctx, expr->queryChild(0), lhs);
    buildCachedExpr(ctx, expr->queryChild(1), rhs);
    buildCachedExpr(ctx, expr->queryChild(2), locale);
    buildCachedExpr(ctx, expr->queryChild(3), strength);
    Owned<IHqlExpression> op;
    HqlExprArray args;
    ITypeInfo * realType = lhs.expr->queryType()->queryPromotedType();
    switch(realType->getTypeCode())
    {
    case type_unicode:
        args.append(*getBoundLength(lhs));
        args.append(*getElementPointer(lhs.expr));
        args.append(*getBoundLength(rhs));
        args.append(*getElementPointer(rhs.expr));
        args.append(*getElementPointer(locale.expr));
        args.append(*strength.expr.getLink());
        op.setown(bindTranslatedFunctionCall(compareUnicodeUnicodeStrengthAtom, args));
        break;
    case type_varunicode:
        args.append(*getElementPointer(lhs.expr));
        args.append(*getElementPointer(rhs.expr));
        args.append(*getElementPointer(locale.expr));
        args.append(*strength.expr.getLink());
        op.setown(bindTranslatedFunctionCall(compareVUnicodeVUnicodeStrengthAtom, args));
        break;
    case type_utf8:
        args.append(*getBoundLength(lhs));
        args.append(*getElementPointer(lhs.expr));
        args.append(*getBoundLength(rhs));
        args.append(*getElementPointer(rhs.expr));
        args.append(*getElementPointer(locale.expr));
        args.append(*strength.expr.getLink());
        op.setown(bindTranslatedFunctionCall(compareUtf8Utf8StrengthAtom, args));
        break;
    default:
        UNIMPLEMENTED;
    }

    assignBound(ctx, target, op);
}

//---------------------------------------------------------------------------
//-- no_order --

static void buildIteratorFirst(HqlCppTranslator & translator, BuildCtx & ctx, IHqlExpression * iter, IHqlExpression * row)
{
    StringBuffer s;
    translator.generateExprCpp(s, row).append(" = (byte*)");
    translator.generateExprCpp(s, iter).append(".first();");
    ctx.addQuoted(s);
}

static void buildIteratorNext(HqlCppTranslator & translator, BuildCtx & ctx, IHqlExpression * iter, IHqlExpression * row)
{
    StringBuffer s;
    translator.generateExprCpp(s, row).append(" = (byte*)");
    translator.generateExprCpp(s, iter).append(".next();");
    ctx.addQuoted(s);
}

static void buildIteratorIsValid(BuildCtx & ctx, IHqlExpression * iter, IHqlExpression * row, CHqlBoundExpr & bound)
{
    bound.expr.set(row);
}

void HqlCppTranslator::doBuildAssignCompareRow(BuildCtx & ctx, EvaluateCompareInfo & info, IHqlExpression * left, IHqlExpression * right)
{
    HqlExprArray leftValues, rightValues;
    IHqlExpression * record = left->queryRecord();
    expandRowOrder(left->queryNormalizedSelector(), record, leftValues, false);
    expandRowOrder(right->queryNormalizedSelector(), record, rightValues, false);
    optimizeOrderValues(leftValues, rightValues, false);

    doBuildAssignCompare(ctx, info, leftValues, rightValues, true, false);      //MORE: ,no_break,true
}

void HqlCppTranslator::doBuildAssignCompareTable(BuildCtx & ctx, EvaluateCompareInfo & info, IHqlExpression * left, IHqlExpression * right)
{
    ITypeInfo * targetType = info.target.queryType();
    OwnedHqlExpr zeroTarget = createConstant(targetType->castFrom(true, 0));
    OwnedHqlExpr plusOne = createConstant(targetType->castFrom(true, +1));
    OwnedHqlExpr minusOne = createConstant(targetType->castFrom(true, -1));

    // cmp = 0;
    assignBound(ctx, info.target, zeroTarget);

    BuildCtx subctx(ctx);
    subctx.addGroup();      // stop bound cursors leaking outside the testing block.

    // i1 iter1; i1.first();
    HqlExprAttr leftIter, leftRow;
    Owned<IHqlCppDatasetCursor> cursor = createDatasetSelector(subctx, left);
    cursor->buildIterateClass(subctx, leftIter, leftRow);
    buildIteratorFirst(*this, subctx, leftIter, leftRow);

    // i2; forEachIn(i2); {
    CHqlBoundExpr isValid;
    BuildCtx loopctx(subctx);
    buildDatasetIterate(loopctx, right, true);
    bindTableCursor(loopctx, left, leftRow);
 
    //     if (!i1.isValid()) { cmp = -1; break; }
    buildIteratorIsValid(loopctx, leftIter, leftRow, isValid);
    OwnedHqlExpr test = createValue(no_not, makeBoolType(), isValid.expr.getClear());
    BuildCtx moreRightCtx(loopctx);
    moreRightCtx.addFilter(test);

    if (info.actionIfDiffer == return_stmt)
    {
        if (info.isEqualityCompare())
        {
            OwnedHqlExpr returnValue = info.getEqualityReturnValue();
            moreRightCtx.addReturn(returnValue);
        }
        else
            moreRightCtx.addReturn(minusOne);
    }
    else
    {
        buildExprAssign(moreRightCtx, info.target, minusOne);
        moreRightCtx.addBreak();
    }

    //Now do the comparison....
    {
        EvaluateCompareInfo childInfo(info);
        if (childInfo.actionIfDiffer == break_stmt)
            childInfo.actionIfDiffer = null_stmt;
        //***childInfo??
        doBuildAssignCompareRow(loopctx, info, left, right);
    }

    if (info.actionIfDiffer != return_stmt)
    {
        //     if (cmp != 0) break;
        BuildCtx donectx(loopctx);
        donectx.addFilter(info.target.expr);
        donectx.addQuoted("break;");
    }

    //     i1.next();
    buildIteratorNext(*this, loopctx, leftIter, leftRow);

    buildIteratorIsValid(subctx, leftIter, leftRow, isValid);
    if (info.actionIfDiffer == return_stmt)
    {
        //if (i1.isValid) return +1;
        BuildCtx moreLeftCtx(subctx);
        moreLeftCtx.addFilter(isValid.expr);
        if (info.isEqualityCompare())
        {
            OwnedHqlExpr returnValue = info.getEqualityReturnValue();
            moreLeftCtx.addReturn(returnValue);
        }
        else
            moreLeftCtx.addReturn(plusOne);
    }
    else
    {
        //if (cmp == 0 && i1.isValid) cmp = +1;
        OwnedHqlExpr cmp = createBoolExpr(no_and, createBoolExpr(no_eq, LINK(info.target.expr), LINK(zeroTarget)), LINK(isValid.expr));
        BuildCtx moreLeftCtx(subctx);
        moreLeftCtx.addFilter(cmp);
        buildExprAssign(moreLeftCtx, info.target, plusOne);

        BuildCtx tailctx(ctx);
        if (info.actionIfDiffer == break_stmt)
        {
            tailctx.addFilter(info.target.expr);
            tailctx.addBreak();
        }
    }
}


void HqlCppTranslator::expandRowOrder(IHqlExpression * selector, IHqlExpression * record, HqlExprArray & values, bool isRow)
{
    ForEachChild(idx, record)
    {
        IHqlExpression * field = record->queryChild(idx);

        switch (field->getOperator())
        {
        case no_ifblock:
            expandRowOrder(selector, field->queryChild(1), values, isRow);
            break;
        case no_record:
            expandRowOrder(selector, field, values, isRow);
            break;
        case no_field:
            {
                OwnedHqlExpr selected;
                if (isRow)
                    selected.setown(createNewSelectExpr(LINK(selector), LINK(field)));
                else
                    selected.setown(createSelectExpr(LINK(selector), LINK(field)));
                if (field->isDatarow())
                    expandRowOrder(selected, field->queryRecord(), values, false);
                else
                    values.append(*LINK(selected));
                break;
            }
        }
    }
}


void HqlCppTranslator::expandSimpleOrder(IHqlExpression * left, IHqlExpression * right, HqlExprArray & leftValues, HqlExprArray & rightValues)
{
    while ((left->getOperator() == no_negate) && (right->getOperator() == no_negate))
    {
        IHqlExpression * temp = right->queryChild(0);
        right = left->queryChild(0);
        left = temp;
    }

    if (left == right)
    {
        //Weird code is here just so I can force some strange exceptions in the regression suite.
        IHqlExpression * cur = left;
        if (cur->getOperator() == no_alias)
            cur = cur->queryChild(0);
        if (cur->getOperator() != no_nofold)
            return;
    }

    if (left->isDatarow())
    {
        IHqlExpression * record = left->queryRecord();
        assertex(right->isDatarow() && (record == right->queryRecord()));
        expandRowOrder(left, record, leftValues, !isActiveRow(left));
        expandRowOrder(right, record, rightValues, !isActiveRow(right));
    }
    else
    {
        leftValues.append(*LINK(left));
        rightValues.append(*LINK(right));
    }
}


void HqlCppTranslator::expandOrder(IHqlExpression * expr, HqlExprArray & leftValues, HqlExprArray & rightValues, OwnedHqlExpr & defaultValue)
{
    OwnedHqlExpr left = normalizeListCasts(expr->queryChild(0));
    OwnedHqlExpr right = normalizeListCasts(expr->queryChild(1));

    if ((isFixedLengthList(left) || isNullList(left)) && (isFixedLengthList(right) || isNullList(right)))
    {
        unsigned maxLeft = left->numChildren();
        unsigned maxRight = right->numChildren();
        unsigned max = std::min(maxLeft, maxRight);
        for (unsigned i=0; i < max; i++)
            expandSimpleOrder(left->queryChild(i), right->queryChild(i), leftValues, rightValues);

        if (maxLeft != maxRight)
            defaultValue.setown(createConstant(signedType->castFrom(true, ((maxLeft > maxRight) ? +1 : -1))));
    }
    else
        expandSimpleOrder(left, right, leftValues, rightValues);
}


IHqlExpression * HqlCppTranslator::querySimpleOrderSelector(IHqlExpression * expr)
{
    if (expr->getOperator() != no_select)
        return NULL;

    bool isNew;
    IHqlExpression * ds = querySelectorDataset(expr, isNew);
    if (isNew)
        return NULL;
    return ds;
}

static unsigned getMemcmpSize(IHqlExpression * left, IHqlExpression * right, bool isEqualityCompare)
{
    ITypeInfo * leftType = left->queryType();
    if (!leftType)
        return 0;

    if (!isSameBasicType(leftType, right->queryType()))
        return 0;

    unsigned size = leftType->getSize();
    switch (leftType->getTypeCode())
    {
    case type_bigendianint:
    case type_boolean:
        return size;
    case type_littleendianint:
        if ((size == 1) && !leftType->isSigned())
            return 1;
        if (isEqualityCompare)
            return size;
        break;
    case type_string:
    case type_data:
    case type_qstring:
        if (size != UNKNOWN_LENGTH)
            return size;
        break;
    }
    return 0;
}

void HqlCppTranslator::optimizeOrderValues(HqlExprArray & leftValues, HqlExprArray & rightValues, bool isEqualityCompare)
{
    unsigned max = leftValues.ordinality();
    if (max <= 1)
        return;
    for (unsigned i=0; i < max-1; i++)
    {
        IHqlExpression * curFirstLeft = &leftValues.item(i);
        IHqlExpression * curFirstRight = &rightValues.item(i);
        IHqlExpression * leftSel = querySimpleOrderSelector(curFirstLeft);
        IHqlExpression * rightSel = querySimpleOrderSelector(curFirstRight);
        if (!leftSel || !rightSel)
            continue;

        unsigned compareSize = getMemcmpSize(curFirstLeft, curFirstRight, isEqualityCompare);
        if (!compareSize)
            continue;

        IHqlExpression * nextLeft = &leftValues.item(i+1);
        IHqlExpression * nextRight = &rightValues.item(i+1);
        if (querySimpleOrderSelector(nextLeft) != leftSel || querySimpleOrderSelector(nextRight) != rightSel ||
            (getMemcmpSize(nextLeft, nextRight, isEqualityCompare) == 0))
            continue;

        //Worth iterating the selectors...
        RecordSelectIterator leftIter(leftSel->queryRecord(), leftSel);
        ForEach(leftIter)
            if (leftIter.query() == curFirstLeft)
                break;
        if (!leftIter.isValid() || leftIter.isInsideIfBlock())
            continue;

        RecordSelectIterator rightIter(rightSel->queryRecord(), rightSel);
        ForEach(rightIter)
            if (rightIter.query() == curFirstRight)
                break;
        if (!rightIter.isValid() || rightIter.isInsideIfBlock())
            continue;

        unsigned j;  // linux wants it declared outside of 'for'
        for (j=i+1; j < max; j++)
        {
            if (!leftIter.next() || leftIter.isInsideIfBlock())
                break;
            if (!rightIter.next() || rightIter.isInsideIfBlock())
                break;
            IHqlExpression * nextLeft = &leftValues.item(j);
            IHqlExpression * nextRight = &rightValues.item(j);
            if (leftIter.query() != nextLeft || rightIter.query() != nextRight)
                break;
            unsigned thisSize = getMemcmpSize(nextLeft, nextRight, isEqualityCompare);
            if (!thisSize)
                break;
            compareSize += thisSize;
        }

        if (j != i+1)
        {
            IHqlExpression * newLeft = createValue(no_typetransfer, makeStringType(compareSize), LINK(curFirstLeft));
            IHqlExpression * newRight = createValue(no_typetransfer, makeStringType(compareSize), LINK(curFirstRight));
            leftValues.replace(*newLeft, i);
            rightValues.replace(*newRight, i);
            leftValues.removen(i+1, j-(i+1));
            rightValues.removen(i+1, j-(i+1));
            max -= (j - (i+1));
        }
    }
}


inline IHqlExpression * createSignedConstant(__int64 value)
{
    return createConstant(signedType->castFrom(true, value));
}

static IHqlExpression * convertAllToInteger(IHqlExpression * allExpr)
{
    IValue * allValue = allExpr->queryValue();
    if (allValue)
        return createSignedConstant(allValue->getBoolValue() ? 1 : 0);

    return createValue(no_if, LINK(signedType), LINK(allExpr), createSignedConstant(1), createSignedConstant(0));
}

void HqlCppTranslator::doBuildAssignCompareElement(BuildCtx & ctx, EvaluateCompareInfo & info, IHqlExpression * left, IHqlExpression * right, bool isFirst, bool isLast)
{
    if (left->getOperator() == no_if && right->getOperator() == no_if && left->queryChild(0) == right->queryChild(0))
    {
        BuildCtx subctx(ctx);
        IHqlStmt * filter = buildFilterViaExpr(subctx, left->queryChild(0));
        doBuildAssignCompareElement(subctx, info, left->queryChild(1), right->queryChild(1), isFirst, false);
        if ((isFirst && (info.actionIfDiffer != return_stmt)) || left->queryChild(2) != right->queryChild(2))
        {
            subctx.selectElse(filter);
            doBuildAssignCompareElement(subctx, info, left->queryChild(2), right->queryChild(2), isFirst, false);
        }
        return;
    }
    if (left == right)
    {
        //Can happen from conditions expanded above
        if (isFirst)
        {
            if (info.actionIfDiffer != return_stmt)
                buildExprAssign(ctx, info.target, queryZero());
        }
        return;
    }

    ITypeInfo * leftType = left->queryType();
    type_t tc;
    if (leftType)
        tc = leftType->getTypeCode();
    else
        tc = type_set;

    CHqlBoundExpr lhs,rhs;
    bool useMemCmp = false;
    switch (tc)
    {
    case type_table:
    case type_groupedtable:
        doBuildAssignCompareTable(ctx, info, left, right);
        return;
    case type_row:
        doBuildAssignCompareRow(ctx, info, left, right);
        return;
    case type_bigendianint:
        {
            //MORE: Compare big endian integers with a memcmp
            if (hasAddress(ctx, left) && hasAddress(ctx, right) && isSameBasicType(leftType, right->queryType()))
            {
                buildAddress(ctx, left, lhs);
                buildAddress(ctx, right, rhs);
                useMemCmp = true;
                break;
            }

            Owned<ITypeInfo> type = makeIntType(leftType->getSize(), leftType->isSigned());
            OwnedHqlExpr intLeft = createValue(no_implicitcast, type.getLink(), LINK(left));
            OwnedHqlExpr intRight = createValue(no_implicitcast, type.getLink(), LINK(right));
            buildCachedExpr(ctx, intLeft, lhs);
            buildCachedExpr(ctx, intRight, rhs);
            break;
        }
    case type_string:
    case type_data:
    case type_qstring:
        {
            OwnedHqlExpr simpleLeft = getSimplifyCompareArg(left);
            OwnedHqlExpr simpleRight = getSimplifyCompareArg(right);
            buildCachedExpr(ctx, simpleLeft, lhs);
            buildCachedExpr(ctx, simpleRight, rhs);
            break;
        }
    default:
        buildCachedExpr(ctx, left, lhs);
        buildCachedExpr(ctx, right, rhs);
        break;
    }


    ITypeInfo * realType = realType = lhs.queryType()->queryPromotedType();
    tc = realType->getTypeCode();
    IHqlExpression * op = NULL;
    switch (tc)
    {
        //MORE: Should common up with comparison code...
        case type_string:
        case type_data:
        case type_qstring:
            {
                HqlExprArray args;
                _ATOM func;

                if (lhs.length || rhs.length || needVarStringCompare(realType, rhs.queryType()->queryPromotedType()))
                {
                    //MORE: This does not cope with different padding characters...
                    func = queryStrCompareFunc(realType);
                    args.append(*getBoundLength(lhs));
                    args.append(*getElementPointer(lhs.expr));
                    args.append(*getBoundLength(rhs));
                    args.append(*getElementPointer(rhs.expr));
                }
                else
                {
                    func = memcmpAtom;
                    args.append(*getElementPointer(lhs.expr));
                    args.append(*getElementPointer(rhs.expr));
                    args.append(*getSizetConstant(realType->getSize()));
                }
                
                op = bindTranslatedFunctionCall(func, args);
                break;
            }
        case type_unicode:
            {
                HqlExprArray args;
                assertex(haveCommonLocale(leftType, right->queryType()));
                char const * locale = getCommonLocale(leftType, right->queryType())->str();
                args.append(*getBoundLength(lhs));
                args.append(*getElementPointer(lhs.expr));
                args.append(*getBoundLength(rhs));
                args.append(*getElementPointer(rhs.expr));
                args.append(*createConstant(locale));
                op = bindTranslatedFunctionCall(compareUnicodeUnicodeAtom, args);
                break;
            }
        case type_varunicode:
            {
                HqlExprArray args;
                assertex(haveCommonLocale(leftType, right->queryType()));
                char const * locale = getCommonLocale(leftType, right->queryType())->str();
                args.append(*getElementPointer(lhs.expr));
                args.append(*getElementPointer(rhs.expr));
                args.append(*createConstant(locale));
                op = bindTranslatedFunctionCall(compareVUnicodeVUnicodeAtom, args);
                break;
            }
        case type_utf8:
            {
                HqlExprArray args;
                assertex(haveCommonLocale(leftType, right->queryType()));
                char const * locale = getCommonLocale(leftType, right->queryType())->str();
                args.append(*getBoundLength(lhs));
                args.append(*getElementPointer(lhs.expr));
                args.append(*getBoundLength(rhs));
                args.append(*getElementPointer(rhs.expr));
                args.append(*createConstant(locale));
                op = bindTranslatedFunctionCall(compareUtf8Utf8Atom, args);
                break;
            }
        case type_varstring:
            {
                HqlExprArray args;
                args.append(*getElementPointer(lhs.expr));
                args.append(*getElementPointer(rhs.expr));
                
                op = bindTranslatedFunctionCall(strcmpAtom, args);
                break;
            }
        case type_decimal:
            {
                HqlExprArray args;
                if (!isPushed(lhs) && !isPushed(rhs) && (leftType->queryPromotedType() == right->queryType()->queryPromotedType()))
                {
                    args.append(*getSizetConstant(leftType->queryPromotedType()->getSize()));
                    args.append(*getPointer(lhs.expr));
                    args.append(*getPointer(rhs.expr));
                    op = bindTranslatedFunctionCall(leftType->isSigned() ? DecCompareDecimalAtom : DecCompareUDecimalAtom, args);
                }
                else
                {
                    bool pushedLhs = ensurePushed(ctx, lhs);
                    bool pushedRhs = ensurePushed(ctx, rhs);

                    //NB: Arguments could be pushed in opposite order 1 <=> x *2
                    if (pushedLhs && !pushedRhs)
                        op = bindTranslatedFunctionCall(DecDistinctRAtom, args);
                    else
                        op = bindTranslatedFunctionCall(DecDistinctAtom, args);
                }
                break;
            }
        case type_set:
        case type_array:
            {
                //compare all
                OwnedHqlExpr leftAll = lhs.getIsAll();
                OwnedHqlExpr rightAll = rhs.getIsAll();
                assertex(leftAll && rightAll);
                if (leftAll != rightAll)
                {
                    if (leftAll->queryValue() && rightAll->queryValue())
                    {
                        op = createConstant(leftAll->queryValue()->getIntValue() - rightAll->queryValue()->getIntValue());
                        break;
                    }
                    if (getIntValue(leftAll, false) || getIntValue(rightAll, false))
                    {
                        op = createValue(no_sub, LINK(signedType), convertAllToInteger(leftAll), convertAllToInteger(rightAll));
                        break;
                    }
                }
                if (lhs.expr != rhs.expr)
                {
                    HqlExprArray args;
                    args.append(*getBoundLength(lhs));
                    args.append(*getElementPointer(lhs.expr));
                    args.append(*getBoundLength(rhs));
                    args.append(*getElementPointer(rhs.expr));
                    op = bindTranslatedFunctionCall(compareDataDataAtom, args);
                }
                if (leftAll != rightAll)
                {
                    IHqlExpression * orderAll = createValue(no_sub, LINK(signedType), convertAllToInteger(leftAll), convertAllToInteger(rightAll));
                    if (op)
                    {
                        IHqlExpression * cond = NULL;
                        if (!getIntValue(leftAll, true))
                        {
                            if (!getIntValue(rightAll, true))
                                cond = NULL;
                            else
                                cond = LINK(rightAll);
                        }
                        else
                        {
                            if (!getIntValue(rightAll, true))
                                cond = LINK(leftAll);
                            else
                                cond = createBoolExpr(no_or, LINK(leftAll), LINK(rightAll));
                        }
                        if (cond)
                            op = createValue(no_if, LINK(signedType), cond, orderAll, op);
                        else
                            orderAll->Release();
                    }
                    else
                        op = orderAll;
                }
                if (!op)
                    op = getZero();
                break;
            }
        case type_boolean:
        case type_swapint:
        case type_packedint:
        case type_int:
            if (!useMemCmp && !info.isEqualityCompare() && (realType->getSize() < signedType->getSize()))
            {
                op = createValue(no_sub, LINK(signedType), 
                            createValue(no_implicitcast, LINK(signedType), lhs.expr.getLink()),
                            createValue(no_implicitcast, LINK(signedType), rhs.expr.getLink()));
                break;
            }
            //fall through

        default:
            if (useMemCmp)
            {
                HqlExprArray args;
                args.append(*lhs.expr.getLink());
                args.append(*rhs.expr.getLink());
                args.append(*getSizetConstant(leftType->getSize()));
                op = bindTranslatedFunctionCall(memcmpAtom, args);
            }
            else
            {
                if (info.isEqualityCompare())
                {
                    op = createBoolExpr(no_ne, LINK(lhs.expr), LINK(rhs.expr));
                }
                else
                {
                    ensureSimpleExpr(ctx, lhs);
                    ensureSimpleExpr(ctx, rhs);

                    OwnedHqlExpr testlt = createBoolExpr(no_lt, lhs.expr.getLink(), rhs.expr.getLink());
                    OwnedHqlExpr retlt = createIntConstant(-1);
                    OwnedHqlExpr testgt = createBoolExpr(no_gt, lhs.expr.getLink(), rhs.expr.getLink());
                    OwnedHqlExpr retgt = createIntConstant(+1);
                    if (info.actionIfDiffer == return_stmt)
                    {
                        BuildCtx subctx1(ctx);
                        subctx1.addFilter(testlt);
                        subctx1.addReturn(retlt);

                        BuildCtx subctx2(ctx);
                        subctx2.addFilter(testgt);
                        subctx2.addReturn(retgt);
                        return;
                    }
                    else
                    {
                        // generate (a < b ? -1 : (a > b ? +1 : 0))
                        op = createValue(no_if, LINK(signedType),
                                LINK(testlt), LINK(retlt), 
                                    createValue(no_if, LINK(signedType), LINK(testgt), LINK(retgt), getZero()));
                    }
                }
            }
            break;
    }

    OwnedHqlExpr safeReleaseOp = op;
    BuildCtx subctx(ctx);
    if (info.isEqualityCompare() && (info.actionIfDiffer == return_stmt))
    {
        subctx.addFilter(op);
        OwnedHqlExpr returnValue = info.getEqualityReturnValue();
        subctx.addReturn(returnValue);
    }
    else
    {
        assignBound(subctx, info.target, op);
        switch (info.actionIfDiffer)
        {
        case break_stmt:
            subctx.addFilter(info.target.expr);
            subctx.addBreak();
            break;
        case return_stmt:
            if (!isLast || info.hasDefault)
                subctx.addFilter(info.target.expr);
            else
                info.alwaysReturns = true;
            subctx.addReturn(info.target.expr);
            break;
        }
    }
}


void HqlCppTranslator::doBuildAssignCompare(BuildCtx & ctx, EvaluateCompareInfo & info, HqlExprArray & leftValues, HqlExprArray & rightValues, bool isFirst, bool isOuter)
{
    assertex(leftValues.ordinality() == rightValues.ordinality());
    Owned<BuildCtx> subctx = new BuildCtx(ctx);

    OwnedHqlExpr compare = createBoolExpr(no_not, LINK(info.target.expr));
    unsigned idx;
    unsigned max = leftValues.ordinality();
    for (idx = 0; idx < max; idx++)
    {
        if ((idx & 7) == 0)
            subctx.setown(new BuildCtx(ctx));

        if (!info.actionIfDiffer && !isFirst)
            subctx->addFilter(compare);

        doBuildAssignCompareElement(*subctx, info, &leftValues.item(idx), &rightValues.item(idx), isFirst, isOuter && (idx == max-1));
        isFirst = false;
    }
}

void HqlCppTranslator::doBuildAssignOrder(BuildCtx & ctx, const CHqlBoundTarget & target, IHqlExpression * expr)
{
    HqlExprArray leftValues, rightValues;
    OwnedHqlExpr defaultValue;
    expandOrder(expr, leftValues, rightValues, defaultValue);
    optimizeOrderValues(leftValues, rightValues, false);

    EvaluateCompareInfo info(no_order);
    info.target.set(target);
    doBuildAssignCompare(ctx, info, leftValues, rightValues, true, false);

    unsigned maxLeft = leftValues.ordinality();
    if (defaultValue)
    {
        if (maxLeft != 0)
        {
            OwnedHqlExpr compare = createBoolExpr(no_not, LINK(target.expr));
            ctx.addFilter(compare);
        }
        buildExprAssign(ctx, target, defaultValue);
    }
    else if (maxLeft == 0)
        buildExprAssign(ctx, target, queryZero());
}

void HqlCppTranslator::doBuildReturnCompare(BuildCtx & ctx, IHqlExpression * expr, node_operator op, bool isBoolEquality)
{
    HqlExprArray leftValues, rightValues;
    OwnedHqlExpr defaultValue;
    expandOrder(expr, leftValues, rightValues, defaultValue);
    optimizeOrderValues(leftValues, rightValues, (op == no_eq));

    EvaluateCompareInfo info(op);
    info.actionIfDiffer = return_stmt;
    info.isBoolEquality = isBoolEquality;
    info.hasDefault = (defaultValue != NULL);

    createTempFor(ctx, expr, info.target);

    doBuildAssignCompare(ctx, info, leftValues, rightValues, true, true);

    if (!info.alwaysReturns)
    {
        if (info.isBoolEquality)
        {
            OwnedHqlExpr returnValue = createConstant(defaultValue == NULL);
            buildReturn(ctx, returnValue);
        }
        else
        {
            if (defaultValue)
                buildReturn(ctx, defaultValue);
            else
                buildReturn(ctx, queryZero());
        }
    }
}

//---------------------------------------------------------------------------
//-- no_hash --

class HashCodeCreator
{
public:
    HashCodeCreator(HqlCppTranslator & _translator, const CHqlBoundTarget & _target, node_operator _hashKind, bool _optimizeInternal) 
        : translator(_translator), target(_target), hashKind(_hashKind), optimizeInternal(_optimizeInternal)
    {
        prevFunc = NULL;
    }

    //Combine calls to the hash function on adjacent memory to minimise the number of calls
    //and the generated code size.
    void buildHash(BuildCtx & ctx, _ATOM func, IHqlExpression * length, IHqlExpression * ptr)
    {
        if ((func == hash32DataAtom) || (func == hash64DataAtom))
        {
            ptr = stripTranslatedCasts(ptr);
            if (prevFunc)
            {
                if ((prevFunc == func) && rightFollowsLeft(prevPtr, prevLength, ptr))
                {
                    prevLength.setown(peepholeAddExpr(prevLength, length));
                    return;
                }
                flush(ctx);
            }

            prevFunc = func;
            prevLength.set(length);
            prevPtr.set(ptr);
            return;
        }
        flush(ctx);

        buildCall(ctx, func, length, ptr);
    }

    void beginCondition(BuildCtx & ctx)
    {
        ensureInitialAssigned(ctx);
        flush(ctx);
    }

    void endCondition(BuildCtx & ctx)
    {
        flush(ctx);
    }

    void finish(BuildCtx & ctx)
    {
        flush(ctx);
        ensureInitialAssigned(ctx);
    }

    void setInitialValue(IHqlExpression * expr)
    {
        initialValue.set(expr);
    }

    inline node_operator kind() const { return hashKind; }
    inline bool optimize() const { return optimizeInternal; }

protected:
    void buildCall(BuildCtx & ctx, _ATOM func, IHqlExpression * length, IHqlExpression * ptr)
    {
        if (func == hash32DataAtom)
        {
            unsigned fixedSize = (unsigned)getIntValue(length, 0);
            switch (fixedSize)
            {
            case 1: func = hash32Data1Atom; break;
            case 2: func = hash32Data2Atom; break;
            case 3: func = hash32Data3Atom; break;
            case 4: func = hash32Data4Atom; break;
            case 5: func = hash32Data5Atom; break;
            case 6: func = hash32Data6Atom; break;
            case 7: func = hash32Data7Atom; break;
            case 8: func = hash32Data8Atom; break;
            }
            if (func != hash32DataAtom)
                length = NULL;
        }

        HqlExprArray args;
        if (length)
            args.append(*LINK(length));
        args.append(*LINK(ptr));

        if (initialValue)
            args.append(*initialValue.getClear());
        else
            args.append(*LINK(target.expr));

        CHqlBoundExpr boundHash;
        boundHash.expr.setown(translator.bindTranslatedFunctionCall(func, args));
        translator.assign(ctx, target, boundHash);
    }

    void ensureInitialAssigned(BuildCtx & ctx)
    {
        if (initialValue)
        {
            translator.assignBound(ctx, target, initialValue);
            initialValue.clear();
        }
    }

    void flush(BuildCtx & ctx)
    {
        if (prevFunc)
        {
            buildCall(ctx, prevFunc, prevLength, prevPtr);
            prevFunc = NULL;
        }
    }


protected:
    HqlCppTranslator & translator;
    const CHqlBoundTarget & target;
    LinkedHqlExpr initialValue;
    node_operator hashKind;
    bool optimizeInternal;
    _ATOM prevFunc;
    OwnedHqlExpr prevLength;
    OwnedHqlExpr prevPtr;
};

void HqlCppTranslator::doBuildAssignHashCrc(BuildCtx & ctx, const CHqlBoundTarget & target, IHqlExpression * expr)
{
    IHqlExpression * child = expr->queryChild(0);
    ITypeInfo * childType = child->queryType();

    LinkedHqlExpr initialValue = queryZero();
    node_operator op = expr->getOperator();
    if (op == no_hash32)
        initialValue.setown(createConstant(createIntValue(0x811C9DC5, 4, false)));
    else if (op == no_hash64)
        initialValue.setown(createConstant(createIntValue(I64C(0xcbf29ce484222325), 8, false)));

    HashCodeCreator creator(*this, target, op, expr->hasProperty(internalAtom));
    creator.setInitialValue(initialValue);
    if (child->getOperator() != no_sortlist)
        doBuildAssignHashElement(ctx, creator, child);
    else
    {
        unsigned max = child->numChildren();
        unsigned idx;
        for (idx = 0; idx < max; idx++)
            doBuildAssignHashElement(ctx, creator, child->queryChild(idx));
    }
    creator.finish(ctx);
}


void HqlCppTranslator::doBuildAssignHashElement(BuildCtx & ctx, HashCodeCreator & creator, IHqlExpression * elem, IHqlExpression * record)
{
    bool useNewSelector = elem->isDatarow() && ((elem->getOperator() != no_select) || isNewSelector(elem));
    ForEachChild(i, record)
    {
        IHqlExpression * cur = record->queryChild(i);
        switch (cur->getOperator())
        {
        case no_field:
            {
                OwnedHqlExpr selected = useNewSelector ? createNewSelectExpr(LINK(elem), LINK(cur)) : createSelectExpr(LINK(elem), LINK(cur));
                doBuildAssignHashElement(ctx, creator, selected);
                break;
            }
        case no_record:
            doBuildAssignHashElement(ctx, creator, elem, cur);
            break;
        case no_ifblock:
            doBuildAssignHashElement(ctx, creator, elem, cur->queryChild(1));
            break;
        }
    }
}

void HqlCppTranslator::doBuildAssignHashElement(BuildCtx & ctx, HashCodeCreator & creator, IHqlExpression * elem)
{
    if (creator.optimize())
    {
        switch (elem->getOperator())
        {
        case no_if:
            {
                BuildCtx subctx(ctx);
                creator.beginCondition(subctx);
                IHqlStmt * cond = buildFilterViaExpr(subctx, elem->queryChild(0));
                doBuildAssignHashElement(subctx, creator, elem->queryChild(1));
                creator.endCondition(subctx);
                IHqlExpression * elseValue = elem->queryChild(2);
                if (elseValue && elseValue->getOperator() != no_constant)
                {
                    subctx.selectElse(cond);
                    creator.beginCondition(subctx);
                    doBuildAssignHashElement(subctx, creator, elseValue);
                    creator.endCondition(subctx);
                }
                return;
            }
        case no_constant:
            return;
        }
    }

    Linked<ITypeInfo> type = elem->queryType()->queryPromotedType();        // skip alien data types, to logical type.
    if (type->getTypeCode() == type_row)
    {
        doBuildAssignHashElement(ctx, creator, elem, elem->queryRecord());
        return;
    }

    _ATOM func=NULL;
    switch (creator.kind())
    {
    case no_hash:   func = hashDataAtom; break;
    case no_hash32: func = hash32DataAtom; break;
    case no_hash64: func = hash64DataAtom; break;
    case no_crc:    func = crcDataAtom; break;
    }

    CHqlBoundExpr bound;
    OwnedHqlExpr length;
    OwnedHqlExpr ptr;
    switch (type->getTypeCode())
    {
        case type_string:
            {
                if (!creator.optimize())
                {
                    OwnedHqlExpr trimmed = createValue(no_trim, getStretchedType(UNKNOWN_LENGTH, type), LINK(elem));
                    buildCachedExpr(ctx, trimmed, bound);
                }
                else
                    buildCachedExpr(ctx, elem, bound);

                length.setown(getBoundLength(bound));
                ptr.setown(getElementPointer(bound.expr));
            }
            break;

        case type_unicode:
            {
                if (!creator.optimize())
                {
                    OwnedHqlExpr trimmed = createValue(no_trim, getStretchedType(UNKNOWN_LENGTH, type), LINK(elem));
                    buildCachedExpr(ctx, trimmed, bound);
                }
                else
                    buildCachedExpr(ctx, elem, bound);
                length.setown(getBoundLength(bound));
                ptr.setown(getElementPointer(bound.expr));
                switch (creator.kind())
                {
                case no_hash:   func = hashUnicodeAtom; break;
                case no_hash32: func = hash32UnicodeAtom; break;
                case no_hash64: func = hash64UnicodeAtom; break;
                case no_crc:    func = crcUnicodeAtom; break;
                }
            }
            break;

        case type_utf8:
            {
                if (!creator.optimize())
                {
                    OwnedHqlExpr trimmed = createValue(no_trim, getStretchedType(UNKNOWN_LENGTH, type), LINK(elem));
                    buildCachedExpr(ctx, trimmed, bound);
                }
                else
                    buildCachedExpr(ctx, elem, bound);
                length.setown(getBoundLength(bound));
                ptr.setown(getElementPointer(bound.expr));
                switch (creator.kind())
                {
                case no_hash:   func = hashUtf8Atom; break;
                case no_hash32: func = hash32Utf8Atom; break;
                case no_hash64: func = hash64Utf8Atom; break;
                case no_crc:    func = crcUtf8Atom; break;
                }
            }
            break;


        case type_data:
        case type_qstring:
            buildCachedExpr(ctx, elem, bound);
            length.setown(getBoundSize(bound));
            ptr.setown(getElementPointer(bound.expr));
            break;

        case type_varstring:
            buildCachedExpr(ctx, elem, bound);
            ptr.setown(getElementPointer(bound.expr));
            switch (creator.kind())
            {
            case no_hash:   func = hashVStrAtom; break;
            case no_hash32: func = hash32VStrAtom; break;
            case no_hash64: func = hash64VStrAtom; break;
            case no_crc:    func = crcVStrAtom; break;
            }
            break;

        case type_varunicode:
            buildCachedExpr(ctx, elem, bound);
            ptr.setown(getElementPointer(bound.expr));
            switch (creator.kind())
            {
            case no_hash:   func = hashVUnicodeAtom; break;
            case no_hash32: func = hash32VUnicodeAtom; break;
            case no_hash64: func = hash64VUnicodeAtom; break;
            case no_crc:    func = crcVUnicodeAtom; break;
            }
            break;

        case type_boolean:
        case type_int:
        case type_swapint:
        case type_real:
            if (creator.optimize() && hasAddress(ctx, elem))
            {
                buildAddress(ctx, elem, bound);
                length.setown(getSizetConstant(type->getSize()));
                ptr.setown(LINK(bound.expr));
            }
            else
            {
                if (!creator.optimize())
                    type.setown(makeIntType(8, true));
                OwnedHqlExpr castElem = ensureExprType(elem, type);
                buildTempExpr(ctx, castElem, bound);
                length.setown(getSizetConstant(type->getSize()));
                ptr.setown(getPointer(bound.expr));
            }
            break;
        case type_row:
            throwUnexpected();
            break;
        case type_groupedtable:
        case type_table:
            //MORE: Should be handle this differently, with an iterator for the link counted rows case?
            //Not sure if that is a good idea - we need to be certain we get the same values with
            //LCR rows enabled and disabled.  But this won't be very efficient with lcr rows.
            //fallthrough
            if (creator.optimize() && hasOutOfLineRows(elem->queryType()))
            {
                BuildCtx iterctx(ctx);
                BoundRow * row = buildDatasetIterate(iterctx, elem, false);
                doBuildAssignHashElement(iterctx, creator, elem->queryNormalizedSelector(), elem->queryRecord());
                return;
            }
            else
            {
                OwnedHqlExpr serialized = ::ensureSerialized(elem);
                buildDataset(ctx, serialized, bound, FormatBlockedDataset);
                length.setown(getBoundSize(bound));
                ptr.setown(getPointer(bound.expr));
            }
            break;
        default:
            buildTempExpr(ctx, elem, bound, FormatBlockedDataset);
            length.setown(getBoundSize(bound));
            ptr.setown(getPointer(bound.expr));
            break;
    }

    creator.buildHash(ctx, func, length, ptr);
}


//---------------------------------------------------------------------------
//-- no_hash --

void HqlCppTranslator::doBuildAssignHashMd5(BuildCtx & ctx, const CHqlBoundTarget & target, IHqlExpression * expr)
{
    IHqlExpression * child = expr->queryChild(0);
    ITypeInfo * childType = child->queryType();

    Owned<ITypeInfo> stateType = makeDataType(sizeof(md5_state_s));
    //initialize the state object
    CHqlBoundTarget stateTemp;
    CHqlBoundExpr state;
    createTempFor(ctx, stateType, stateTemp, typemod_none, FormatNatural);
    state.setFromTarget(stateTemp);
    OwnedHqlExpr stateExpr = state.getTranslatedExpr();

    HqlExprArray args;
    args.append(*LINK(stateExpr));
    OwnedHqlExpr callInit = bindFunctionCall(hashMd5InitAtom, args);
    buildStmt(ctx, callInit);

    //Now hash each of the elements in turn.
    if (child->getOperator() != no_sortlist)
        doBuildHashMd5Element(ctx, child, state);
    else
    {
        unsigned max = child->numChildren();
        for (unsigned idx = 0; idx < max; idx++)
            doBuildHashMd5Element(ctx, child->queryChild(idx), state);
    }

    //finalise the md5, and get the result.
    args.append(*LINK(stateExpr));
    OwnedHqlExpr callFinish = bindFunctionCall(hashMd5FinishAtom, args);
    buildExprAssign(ctx, target, callFinish);
}


void HqlCppTranslator::doBuildHashMd5Element(BuildCtx & ctx, IHqlExpression * elem, CHqlBoundExpr & state)
{
    CHqlBoundExpr bound;

    Linked<ITypeInfo> type = elem->queryType()->queryPromotedType();        // skip alien data types, to logical type.

    HqlExprArray args;
    _ATOM func=NULL;
    switch (type->getTypeCode())
    {
    case type_string:
    case type_unicode:
    case type_data:
    case type_qstring:
    case type_varstring:
    case type_varunicode:
    case type_utf8:
        buildExpr(ctx, elem, bound);
        args.append(*getBoundSize(bound));
        args.append(*getElementPointer(bound.expr));
        break;
    case type_int:
    case type_swapint:
    case type_packedint:
        {
            type.setown(makeIntType(8, true));
            OwnedHqlExpr castElem = ensureExprType(elem, type);
            buildTempExpr(ctx, castElem, bound);
            args.append(*getSizetConstant(type->getSize()));
            args.append(*getPointer(bound.expr));
            break;
        }
    default:
        buildTempExpr(ctx, elem, bound);
        args.append(*getSizetConstant(type->getSize()));
        args.append(*getPointer(bound.expr));
        break;
    }
    args.append(*getBoundSize(state));
    args.append(*LINK(state.expr));
    OwnedHqlExpr call = bindTranslatedFunctionCall(hashMd5DataAtom, args);
    ctx.addExpr(call);
}


//---------------------------------------------------------------------------

void HqlCppTranslator::doBuildExprTransfer(BuildCtx & ctx, IHqlExpression * expr, CHqlBoundExpr & tgt)
{
    CHqlBoundExpr bound;

    //Ensure the bound result has an address
    IHqlExpression * src = expr->queryChild(0);
    bool gotAddress = false;
    if (src->isDataset())
    {
        buildDataset(ctx, src, bound, FormatBlockedDataset);
        bound.expr.setown(getPointer(bound.expr));
    }
    else if (src->isDatarow())
    {
        Owned<IReferenceSelector> ref = buildNewRow(ctx, src);
        ref->get(ctx, bound);
        bound.expr.setown(getPointer(bound.expr));
    }
    else if (isTypePassedByAddress(src->queryType()))
        buildCachedExpr(ctx, src, bound);
    else if (hasAddress(ctx, src))
    {
        buildAddress(ctx, src, bound);
        gotAddress = true;
    }
    else
        buildTempExpr(ctx, src, bound);

    OwnedITypeInfo from = bound.expr->getType();
    ITypeInfo * to = expr->queryType();

    //Must calculate the size of the bound value before we start messing about with stripping casts etc.
    OwnedHqlExpr size;
    if (to->getSize() == UNKNOWN_LENGTH)
    {
        if (from->getSize() == UNKNOWN_LENGTH)
            size.setown(getBoundSize(bound));
        else
            size.setown(getSizetConstant(from->getSize()));
    }

    if (!isTypePassedByAddress(from) && !gotAddress)
        bound.expr.setown(getAddress(bound.expr));

    //strip unnecessary casts...
    while (bound.expr->getOperator() == no_implicitcast)
        bound.expr.set(bound.expr->queryChild(0));

    if (isTypePassedByAddress(to))
    {
        to->Link();
        if (!to->isReference())
            to = makeReferenceModifier(to);
        tgt.expr.setown(createValue(no_implicitcast, to, LINK(bound.expr)));
        if (to->getSize() == UNKNOWN_LENGTH)
        {
            switch (to->getTypeCode())
            {
            case type_unicode:
                if (size->isConstant())
                    tgt.length.setown(getSizetConstant((size32_t)getIntValue(size)/sizeof(UChar)));
                else
                    tgt.length.setown(createValue(no_div, LINK(sizetType), LINK(size), getSizetConstant(2)));
                break;
            case type_qstring:
                if (size->isConstant())
                    tgt.length.setown(getSizetConstant(rtlQStrLength((size32_t)getIntValue(size))));
                else
                    tgt.length.setown(createValue(no_div, LINK(sizetType), multiplyValue(size, 4), getSizetConstant(3)));
                break;
            case type_varstring:
            case type_varunicode:
                break;
            default:
                tgt.length.set(size);
                break;
            }
        }
    }
    else
    {
        tgt.length.clear();
        tgt.expr.set(bound.expr);
        if (hasWrapperModifier(tgt.expr->queryType()))
            tgt.expr.setown(createValue(no_implicitcast, makeReferenceModifier(LINK(queryUnqualifiedType(from))), LINK(tgt.expr)));
        tgt.expr.setown(createValue(no_implicitcast, makePointerType(LINK(to)), tgt.expr.getClear()));
        tgt.expr.setown(createValue(no_deref, LINK(to), tgt.expr.getClear()));
    }
}

//---------------------------------------------------------------------------
//-- no_ordered

void HqlCppTranslator::doBuildExprOrdered(BuildCtx & ctx, IHqlExpression * expr, CHqlBoundExpr & tgt)
{
    if (ctx.getMatchExpr(expr, tgt))
        return;

    bool ascending = true;
    IHqlExpression * list = expr->queryChild(0);
    IHqlExpression * attr = expr->queryChild(1);
    if (attr && attr->isAttribute() && (attr->queryName() == descAtom))
        ascending = false;
    if (list->numChildren() == 0)
        throwError(HQLERR_RankOnNull);

    //create the list that is going to be sorted
    CHqlBoundExpr boundList;
    OwnedHqlExpr optimalList = getOptimialListFormat(list);
    buildExpr(ctx, optimalList, boundList);

    //create a compare function....
    ITypeInfo * elementType = boundList.expr->queryType()->queryChildType();
    unsigned elementSize = elementType->getSize();
    if (elementSize == UNKNOWN_LENGTH)
        throwError(HQLERR_OrderOnVarlengthStrings);

    StringBuffer tempName;
    getUniqueId(tempName.append('v'));
    IHqlExpression * tempCompare = createVariable(tempName.str(), makeVoidType());

    StringBuffer s;
    s.clear().append("extern int ").append(tempName).append("(const void * left, const void * right);");
    if (options.spanMultipleCpp)
    {
        BuildCtx protoctx(*code, mainprototypesAtom);
        protoctx.addQuoted(s);
    }
    else
    {
        BuildCtx protoctx(*code, prototypeAtom);
        protoctx.addQuoted(s);
    }

    BuildCtx declareCtx(*code, declareAtom);
    s.clear().append("int ").append(tempName).append("(const void * left, const void * right)");
    declareCtx.addQuotedCompound(s);

    Owned<ITypeInfo> argType;
    if (isTypePassedByAddress(elementType) && !hasReferenceModifier(elementType))
        argType.setown(makeReferenceModifier(LINK(elementType)));
    else
        argType.setown(makePointerType(LINK(elementType)));

    OwnedHqlExpr leftAddr = createVariable("left", LINK(argType));
    OwnedHqlExpr rightAddr = createVariable("right", LINK(argType));
    IHqlExpression * left = convertAddressToValue(leftAddr, elementType);
    IHqlExpression * right = convertAddressToValue(rightAddr, elementType);
    if (elementType->isReference())
        elementSize = sizeof(char * *);

    left = createTranslatedOwned(left);
    right = createTranslatedOwned(right);
    OwnedHqlExpr compare;
    if (ascending)
        compare.setown(createValue(no_order, LINK(signedType), left, right));
    else
        compare.setown(createValue(no_order, LINK(signedType), right, left));
    CHqlBoundExpr boundCompare;
    buildExpr(declareCtx, compare, boundCompare);
    declareCtx.setNextDestructor();
    declareCtx.addReturn(boundCompare.expr);

    //Allocate an array to store the orders
    unsigned max = list->numChildren();

    Owned<ITypeInfo> t = makeArrayType(LINK(unsignedType), max);
    IHqlExpression * table = ctx.getTempDeclare(t, NULL);

    ctx.associateExpr(expr, table);

    //Generate the call to the function that calculates the orders
    IHqlExpression * castCompare = createValue(no_implicitcast, makePointerType(makeVoidType()), tempCompare);
    HqlExprArray args;
    args.append(*getPointer(table));
    args.append(*getPointer(boundList.expr));
    args.append(*createConstant(unsignedType->castFrom(false, max)));
    args.append(*getSizetConstant(elementSize));
    args.append(*castCompare);
    callProcedure(ctx, createOrderAtom, args);
    tgt.expr.setown(table);
}

//---------------------------------------------------------------------------
//-- no_rank

void checkRankRange(IHqlExpression * index, IHqlExpression * list)
{
    IValue * indexValue = index->queryValue();
    if (indexValue)
    {
        unsigned max = list->numChildren();
        unsigned idx = (unsigned)indexValue->getIntValue();
        //MORE: Should be a warning.....
        if ((idx < 1) || (idx > max))
            throwError(HQLERR_RankOutOfRange);
    }
    //MORE: Could dynamically allocate the array indexes...
    if (list->getOperator() == no_getresult)
    {
        StringBuffer s;
        IHqlExpression * sequence = queryPropertyChild(list, sequenceAtom, 0);
        IHqlExpression * name = queryPropertyChild(list, namedAtom, 0);
        getStoredDescription(s, sequence, name, true);
        throwError1(HQLERR_RankOnStored, s.str());
    }
}

void HqlCppTranslator::createOrderList(BuildCtx & ctx, IHqlExpression * expr, IHqlExpression * ascdesc, CHqlBoundExpr & tgt)
{
    ITypeInfo * orderedType = makeArrayType(LINK(unsignedType), expr->numChildren());
    OwnedHqlExpr ordered = createValue(no_ordered, orderedType, LINK(expr), LINK(ascdesc));
    buildExpr(ctx, ordered, tgt);
}

void HqlCppTranslator::doBuildExprRank(BuildCtx & ctx, IHqlExpression * expr, CHqlBoundExpr & tgt)
{
    IHqlExpression * index = expr->queryChild(0);
    IHqlExpression * list = expr->queryChild(1);
    checkRankRange(index, list);

    CHqlBoundExpr bound, boundIndex;
    createOrderList(ctx, list, expr->queryChild(2), bound);
    buildExpr(ctx, index, boundIndex);

    HqlExprArray args;
    args.append(*boundIndex.expr.getClear());
    args.append(*createConstant(unsignedType->castFrom(false, list->numChildren())));
    args.append(*getPointer(bound.expr));
    tgt.expr.setown(bindTranslatedFunctionCall(rankFromOrderAtom, args));
}

//---------------------------------------------------------------------------
//-- no_ranked

void HqlCppTranslator::doBuildExprRanked(BuildCtx & ctx, IHqlExpression * expr, CHqlBoundExpr & tgt)
{
    IHqlExpression * index = expr->queryChild(0);
    IHqlExpression * list = expr->queryChild(1);
    checkRankRange(index, list);

    CHqlBoundExpr bound, boundIndex;
    createOrderList(ctx, list, expr->queryChild(2), bound);
    buildExpr(ctx, index, boundIndex);

    HqlExprArray args;
    args.append(*boundIndex.expr.getClear());
    args.append(*createConstant(unsignedType->castFrom(false, list->numChildren())));
    args.append(*getPointer(bound.expr));
    tgt.expr.setown(bindTranslatedFunctionCall(rankedFromOrderAtom, args));
}

//---------------------------------------------------------------------------
//-- no_fail

void HqlCppTranslator::doBuildStmtFail(BuildCtx & ctx, IHqlExpression * expr)
{
    HqlExprArray args;
    args.append(*getFailCode(expr));
    args.append(*getFailMessage(expr, false));
    _ATOM func = expr->hasProperty(defaultAtom) ? sysFailAtom : _failIdAtom;
    OwnedHqlExpr call = bindFunctionCall(func, args);
    buildStmt(ctx, call);
}

void HqlCppTranslator::doBuildExprFailCode(BuildCtx & ctx, IHqlExpression * expr, CHqlBoundExpr & tgt)
{
    OwnedHqlExpr activeFailMarker = createAttribute(activeFailureAtom);
    HqlExprAssociation * matchedMarker = ctx.queryMatchExpr(activeFailMarker);
    if (!matchedMarker && !ctx.queryMatchExpr(globalContextMarkerExpr))
    {
        if (!buildExprInCorrectContext(ctx, expr, tgt, false))
            throwError1(HQLERR_FailXUsedOutsideFailContext, getOpString(expr->getOperator()));
        return;
    }

    HqlExprArray args;
    if (matchedMarker)
    {
        args.append(*LINK(matchedMarker->queryExpr()));
        tgt.expr.setown(bindTranslatedFunctionCall(queryLocalFailCodeAtom, args));
    }
    else
    {
        tgt.expr.setown(bindTranslatedFunctionCall(queryFailCodeAtom, args));
    }
}


void HqlCppTranslator::doBuildAssignFailMessage(BuildCtx & ctx, const CHqlBoundTarget & target, IHqlExpression * expr)
{
    OwnedHqlExpr activeFailMarker = createAttribute(activeFailureAtom);
    HqlExprAssociation * matchedMarker = ctx.queryMatchExpr(activeFailMarker);
    if (!matchedMarker && !ctx.queryMatchExpr(globalContextMarkerExpr))
    {
        CHqlBoundExpr match;
        if (!buildExprInCorrectContext(ctx, expr, match, false))
            throwError1(HQLERR_FailXUsedOutsideFailContext, getOpString(expr->getOperator()));
        assign(ctx, target, match);
        return;
    }

    _ATOM func = getFailMessageAtom;
    HqlExprArray args;
    if (matchedMarker)
    {
        func = getLocalFailMessageAtom;
        args.append(*createTranslated(matchedMarker->queryExpr()));
    }

    LinkedHqlExpr tag = expr->queryChild(0);
    if (!tag)
        tag.setown(createQuoted("0", makeConstantModifier(makeReferenceModifier(makeVarStringType(UNKNOWN_LENGTH, 0, 0)))));
    args.append(*LINK(tag));
    OwnedHqlExpr call = bindFunctionCall(func, args);
    buildExprAssign(ctx, target, call);
}

void HqlCppTranslator::doBuildAssignEventName(BuildCtx & ctx, const CHqlBoundTarget & target, IHqlExpression * expr)
{
    HqlExprArray args;
    OwnedHqlExpr call = bindFunctionCall(getEventNameAtom, args);
    buildExprAssign(ctx, target, call);
}

void HqlCppTranslator::doBuildAssignEventExtra(BuildCtx & ctx, const CHqlBoundTarget & target, IHqlExpression * expr)
{
    LinkedHqlExpr tag = expr->queryChild(0);
    if (!tag)
        tag.setown(createQuoted("0", makeConstantModifier(makeReferenceModifier(makeVarStringType(UNKNOWN_LENGTH, 0, 0)))));

    HqlExprArray args;
    args.append(*LINK(tag));
    OwnedHqlExpr call = bindFunctionCall(getEventExtraAtom, args);
    buildExprAssign(ctx, target, call);
}

//---------------------------------------------------------------------------
//-- system call e.g. EXP(), LOG()...

void HqlCppTranslator::doBuildExprSysFunc(BuildCtx & ctx, IHqlExpression * expr, CHqlBoundExpr & tgt, IAtom * funcName)
{
    HqlExprArray args;
    ForEachChild(i, expr)
    {
        IHqlExpression * cur = expr->queryChild(i);
        if (!cur->isAttribute())
            args.append(*LINK(cur));
    }
    OwnedHqlExpr call = bindFunctionCall(funcName, args);
    buildExpr(ctx, call, tgt);
}

void HqlCppTranslator::doBuildExprOffsetOf(BuildCtx & ctx, IHqlExpression * expr, CHqlBoundExpr & tgt)
{
    if (ctx.getMatchExpr(expr, tgt))
        return;

    IHqlExpression * arg = expr->queryChild(0);
    Owned<IReferenceSelector> selector = buildActiveReference(ctx, arg);
    selector->getOffset(ctx, tgt);

    //cache non-constant values in a temporary variable...
    if (!isSimpleLength(tgt.expr))
    {
        IHqlExpression * temp = ctx.getTempDeclare(expr->queryType(), tgt.expr);
        tgt.expr.setown(temp);
        ctx.associateExpr(expr, tgt);
    }
}

//---------------------------------------------------------------------------
//-- no_subselect --

void HqlCppTranslator::doBuildAssignSubString(BuildCtx & ctx, const CHqlBoundTarget & target, IHqlExpression * expr)
{
    if (expr->queryChild(1)->getOperator() == no_rangecommon)
        throwError(HQLERR_StarRangeOnlyInJoinCondition);

    ITypeInfo * resultType = expr->queryType();
    ITypeInfo * targetType = target.queryType();
    type_t rtc = resultType->getTypeCode();
    type_t ttc = targetType->getTypeCode();

    SubStringInfo info(expr);
    CHqlBoundExpr newBound;
    bool doneAssign = false;

    if (info.special)
        doBuildExprSpecialSubString(ctx, info, newBound);
    else if (info.infiniteString)
        doBuildExprInfiniteSubString(ctx, info, newBound);

    if (!newBound.expr)
    {
        _ATOM func = NULL;
        type_t stc = info.src->queryType()->getTypeCode();
        if (target.isFixedSize())
        {
            switch (ttc)
            {
            case type_qstring:
                if (stc == type_qstring)
                    func = subQStrFTAtom;
                break;
            case type_data:
                switch (stc)
                {
                case type_data:
                case type_string:
                case type_varstring:
                    func = subDataFTAtom;
                    break;
                }
                break;
            case type_string:
                switch (stc)
                {
                case type_data:
                    func = subDataFTAtom;
                    break;
                case type_string:
                case type_varstring:
                    if (resultType->queryCharset() == info.src->queryType()->queryCharset())
                        func = subStrFTAtom;
                    break;
                }
                break;
            }
            if (!func && (queryUnqualifiedType(resultType) != queryUnqualifiedType(targetType)))
            {
                CHqlBoundExpr bound;
                buildTempExpr(ctx, expr, bound);
                assign(ctx, target, bound);
                return;
            }
        }

        CHqlBoundExpr boundSrc;
        buildCachedExpr(ctx, info.src, boundSrc);
        info.bindToFrom(*this, ctx);
        ITypeInfo * sourceType = boundSrc.queryType();

        if (!info.boundFrom.expr)
            info.boundFrom.expr.setown(getSizetConstant(1));

        //Some hacks to force the parameters/return values to the same type.  It could be solved more cleanly,
        //but with more functions by calling different functions instead.
        CHqlBoundTarget tempTarget;
        tempTarget.set(target);
        switch (rtc)
        {
        case type_string:
            if (resultType->queryCollation()->queryName() != asciiAtom)
            {
                unsigned sourceLen = boundSrc.queryType()->getStringLen();
                boundSrc.expr.setown(createValue(no_typetransfer, makeStringType(sourceLen, NULL, NULL), LINK(boundSrc.expr)));
                OwnedITypeInfo newTargetType = makeStringType(targetType->getStringLen(), NULL, NULL);
                tempTarget.expr.setown(createValue(no_typetransfer, cloneModifiers(targetType, newTargetType), LINK(tempTarget.expr)));
            }
            break;
        }

        HqlExprArray args;
        args.append(*boundSrc.getTranslatedExpr());
        args.append(*info.boundFrom.getTranslatedExpr());

        if (func)
        {
            args.add(*createTranslated(tempTarget.expr), 0);
            if (info.boundTo.expr)
                args.append(*info.boundTo.getTranslatedExpr());
            else
                args.append(*createConstant(unsignedType->castFrom(false, 0x7fffffff)));

            OwnedHqlExpr call = bindFunctionCall(func, args);
            buildStmt(ctx, call);
        }
        else
        {
            if (info.boundTo.expr)
            {
                args.append(*info.boundTo.getTranslatedExpr());

                switch (rtc)
                {
                case type_qstring:
                    func = subQStrFTXAtom;
                    break;
                case type_data:
                    func = subDataFTXAtom;
                    break;
                case type_unicode:
                case type_varunicode:
                    func = unicodeSubStrFTXAtom;
                    break;
                case type_utf8:
                    func = utf8SubStrFTXAtom;
                    break;
                default:
                    func = subStrFTXAtom;
                    break;
                }
            }
            else
            {
                switch (rtc)
                {
                case type_qstring:
                    func = subQStrFXAtom;
                    break;
                case type_data:
                    func = subDataFXAtom;
                    break;
                case type_unicode:
                case type_varunicode:
                    func = unicodeSubStrFXAtom;
                    break;
                case type_utf8:
                    func = utf8SubStrFXAtom;
                    break;
                default:
                    func = subStrFXAtom;
                    break;
                }
            }
            OwnedHqlExpr call = bindFunctionCall(func, args);
            buildExprAssign(ctx, tempTarget, call);
        }
        doneAssign = true;
    }

    if (!doneAssign)
        assign(ctx, target, newBound);
}

bool HqlCppTranslator::doBuildExprSpecialSubString(BuildCtx & ctx, SubStringInfo & info, CHqlBoundExpr & tgt)
{
    unsigned size = info.srcType->getStringLen();
    unsigned fromIndex = info.fixedStart;
    unsigned toIndex = info.fixedEnd;

    //If substring is larger than the source use the default processing.
    if (toIndex <= size)
    {
        CHqlBoundExpr boundSrc;
        buildCachedExpr(ctx, info.src, boundSrc);

        boundSrc.expr.setown(getIndexedElementPointer(boundSrc.expr, fromIndex-1));

        unsigned newLength = fromIndex <= toIndex ? toIndex-(fromIndex-1) : 0;
        ITypeInfo * newType = makeReferenceModifier(getStretchedType(newLength, info.srcType));
        tgt.expr.setown(createValue(no_typetransfer, newType, boundSrc.expr.getClear()));

        if (info.expr->queryType()->getStringLen() != newLength)
            tgt.length.setown(getSizetConstant(newLength));
        return true;
    }
    return false;
}

bool HqlCppTranslator::doBuildExprInfiniteSubString(BuildCtx & ctx, SubStringInfo & info, CHqlBoundExpr & tgt)
{
    CHqlBoundExpr boundSrc;
    info.bindToFrom(*this, ctx);
    buildCachedExpr(ctx, info.src, boundSrc);

    IHqlExpression * from = info.from;
    if (info.fixedStart == 1)
        from = NULL;

    IHqlExpression * start = from ? adjustValue(info.boundFrom.expr, -1) : NULL;
    tgt.expr.setown(getIndexedElementPointer(boundSrc.expr, start));
    //ensure type is no longer infinite length, so same optimization does not happen again...
    ITypeInfo * newType = makeReferenceModifier(getStretchedType(UNKNOWN_LENGTH, tgt.expr->queryType()));
    tgt.expr.setown(createValue(no_typetransfer, newType, tgt.expr.getLink()));

    OwnedHqlExpr length;
    if (start && !isZero(start))
        length.setown(createValue(no_sub, info.boundTo.expr.getLink(), LINK(start)));
    else
        length.setown(info.boundTo.expr.getLink());
    tgt.length.setown(ensureExprType(length, sizetType));
    ::Release(start);
    return true;
}

void HqlCppTranslator::doBuildExprAnySubString(BuildCtx & ctx, SubStringInfo & info, CHqlBoundExpr & tgt)
{
    CHqlBoundExpr boundSource;
    buildCachedExpr(ctx, info.src, boundSource);
    info.bindToFrom(*this, ctx);

    OwnedHqlExpr from;
    if (info.from)
    {
        OwnedHqlExpr start = adjustValue(info.boundFrom.expr, -1);
        if (!isZero(start))
        {
            HqlExprArray args;
            args.append(*LINK(start));
            args.append(*getBoundLength(boundSource));
            OwnedHqlExpr call = bindTranslatedFunctionCall(rtlMinAtom, args);
            call.setown(createTranslated(call));
            CHqlBoundExpr fromVar;
            buildTempExpr(ctx, call, fromVar);
            from.set(fromVar.expr);
        }
    }

    OwnedHqlExpr to;
    if (info.to)
    {
        OwnedHqlExpr toExpr = LINK(info.boundTo.expr);
        if (from)
        {
            HqlExprArray args;
            args.append(*LINK(toExpr));
            args.append(*LINK(from));
            toExpr.setown(bindTranslatedFunctionCall(rtlMaxAtom, args));
        }

        HqlExprArray args;
        args.append(*LINK(toExpr));
        args.append(*getBoundLength(boundSource));
        to.setown(bindTranslatedFunctionCall(rtlMinAtom, args));
    }
    else
        to.setown(getBoundLength(boundSource));

    boundSource.expr.setown(getIndexedElementPointer(boundSource.expr, from));

    ITypeInfo * newType = makeReferenceModifier(info.expr->getType());
    tgt.expr.setown(createValue(no_typetransfer, newType, boundSource.expr.getClear()));
    if (from && !isZero(from))
        tgt.length.setown(createValue(no_sub, LINK(sizetType), LINK(to), LINK(from)));
    else
        tgt.length.set(to);
}

void HqlCppTranslator::doBuildExprSubString(BuildCtx & ctx, IHqlExpression * expr, CHqlBoundExpr & tgt)
{
    if (expr->queryChild(1)->getOperator() == no_rangecommon)
        throwError(HQLERR_StarRangeOnlyInJoinCondition);

    /* Optimize string[start..end] into a type transfer where appropriate */
    SubStringInfo info(expr);
    
    if (info.special)
        if (doBuildExprSpecialSubString(ctx, info, tgt))
            return;
        
    if (info.infiniteString)
        if (doBuildExprInfiniteSubString(ctx, info, tgt))
            return;

    if (expr->hasProperty(quickAtom))
    {
        doBuildExprAnySubString(ctx, info, tgt);
        return;
    }

    buildTempExpr(ctx, expr, tgt);
}

//---------------------------------------------------------------------------
//-- no_trim --
void HqlCppTranslator::doBuildAssignTrim(BuildCtx & ctx, const CHqlBoundTarget & target, IHqlExpression * expr) 
{
    IHqlExpression * str = expr->queryChild(0);
    _ATOM func;
    bool hasAll = expr->hasProperty(allAtom);
    bool hasLeft = expr->hasProperty(leftAtom);
    bool hasRight = expr->hasProperty(rightAtom);

    if (str->queryType()->getTypeCode() == type_varstring)
    {
        if(hasAll)
            func = trimVAllAtom;
        else if(hasLeft && hasRight)
            func = trimVBothAtom;
        else if(hasLeft)
            func = trimVLeftAtom;
        else
            func = trimVRightAtom;
    }
    else if(str->queryType()->getTypeCode() == type_unicode)
    {
        if(hasAll)
            func = trimUnicodeAllAtom;
        else if(hasLeft && hasRight)
            func = trimUnicodeBothAtom;
        else if(hasLeft)
            func = trimUnicodeLeftAtom;
        else
            func = trimUnicodeRightAtom;
    }
    else if(str->queryType()->getTypeCode() == type_varunicode)
    {
        if(hasAll)
            func = trimVUnicodeAllAtom;
        else if(hasLeft && hasRight)
            func = trimVUnicodeBothAtom;
        else if(hasLeft)
            func = trimVUnicodeLeftAtom;
        else
            func = trimVUnicodeRightAtom;
    }
    else if(str->queryType()->getTypeCode() == type_utf8)
    {
        if(hasAll)
            func = trimUtf8AllAtom;
        else if(hasLeft && hasRight)
            func = trimUtf8BothAtom;
        else if(hasLeft)
            func = trimUtf8LeftAtom;
        else
            func = trimUtf8RightAtom;
    }
    else
    {
        if(hasAll)
            func = trimAllAtom;
        else if(hasLeft && hasRight)
            func = trimBothAtom;
        else if(hasLeft)
            func = trimLeftAtom;
        else
            func = trimRightAtom;
    }

    HqlExprArray args;
    args.append(*LINK(str));
    OwnedHqlExpr call = bindFunctionCall(func, args);
    buildExprAssign(ctx, target, call);
}


void HqlCppTranslator::doBuildExprTrim(BuildCtx & ctx, IHqlExpression * expr, CHqlBoundExpr & tgt)
{
    // MORE - support LEFT,RIGHT,ALL attributes
    CHqlBoundExpr bound;
    buildSimpleExpr(ctx, expr->queryChild(0), bound);

    HqlExprArray args;
    _ATOM func;
    OwnedHqlExpr str = getElementPointer(bound.expr);
    
    bool hasAll = expr->hasProperty(allAtom);
    bool hasLeft = expr->hasProperty(leftAtom);
    bool hasRight = expr->hasProperty(rightAtom);
    
    if(hasAll || hasLeft) 
    {
        if (bound.expr->queryType()->getTypeCode() == type_varstring)
        {
            if(hasAll) {
                func = trimVAllAtom;
            }
            else if(hasLeft && hasRight) {
                func = trimVBothAtom;
            }
            else {
                func = trimVLeftAtom;
            }
        }
        else if (bound.expr->queryType()->getTypeCode() == type_unicode)
        {
            if(hasAll) {
                func = trimUnicodeAllAtom;
            }
            else if(hasLeft && hasRight) {
                func = trimUnicodeBothAtom;
            }
            else {
                func = trimUnicodeLeftAtom;
            }
        }
        else if (bound.expr->queryType()->getTypeCode() == type_varunicode)
        {
            if(hasAll) {
                func = trimVUnicodeAllAtom;
            }
            else if(hasLeft && hasRight) {
                func = trimVUnicodeBothAtom;
            }
            else {
                func = trimVUnicodeLeftAtom;
            }
        }
        else if (bound.expr->queryType()->getTypeCode() == type_utf8)
        {
            if(hasAll) {
                func = trimUtf8AllAtom;
            }
            else if(hasLeft && hasRight) {
                func = trimUtf8BothAtom;
            }
            else {
                func = trimUtf8LeftAtom;
            }
        }
        else
        {
            if(hasAll) {
                func = trimAllAtom;
            }
            else if(hasLeft && hasRight) {
                func = trimBothAtom;
            }
            else {
                func = trimLeftAtom;
            }
        }

        args.append(*bound.getTranslatedExpr());
        OwnedHqlExpr call = bindFunctionCall(func, args);
        buildExpr(ctx, call, tgt);
    }
    else {
        if (bound.expr->queryType()->getTypeCode() == type_varstring)
        {
            args.append(*LINK(str));
            func = trimVStrLenAtom;
        }
        else if (bound.expr->queryType()->getTypeCode() == type_unicode)
        {
            args.append(*getBoundLength(bound));
            args.append(*LINK(str));
            func = trimUnicodeStrLenAtom;
        }
        else if (bound.expr->queryType()->getTypeCode() == type_varunicode)
        {
            args.append(*LINK(str));
            func = trimVUnicodeStrLenAtom;
        }
        else if (bound.expr->queryType()->getTypeCode() == type_utf8)
        {
            args.append(*getBoundLength(bound));
            args.append(*LINK(str));
            func = trimUtf8StrLenAtom;
        }
        else
        {
            args.append(*getBoundLength(bound));
            args.append(*LINK(str));
            func = trimStrLenAtom;
        }
        tgt.length.setown(bindTranslatedFunctionCall(func, args));
        tgt.expr.set(str);
    }
}

//---------------------------------------------------------------------------

void HqlCppTranslator::doBuildExprIsValid(BuildCtx & ctx, IHqlExpression * expr, CHqlBoundExpr & tgt)
{
    IHqlExpression * const value = expr->queryChild(0);
    HqlExprArray args;

    ITypeInfo * type = value->queryType();
    assertex(type);
    if (type->getTypeCode() == type_alien)
    {
        IHqlAlienTypeInfo * alien = queryAlienType(type);
        IHqlExpression * isValidFunction = alien->queryFunction(getIsValidAtom);
        if (isValidFunction)
        {
            CHqlBoundExpr bound;
            buildAddress(ctx, value, bound);
            
            OwnedITypeInfo physicalType = alien->getPhysicalType();
            if (!isTypePassedByAddress(physicalType))
                bound.expr.setown(createValue(no_deref, makeReferenceModifier(LINK(physicalType)), LINK(bound.expr)));

            HqlExprArray args;
            args.append(*bound.getTranslatedExpr());
            OwnedHqlExpr test = createBoundFunction(NULL, isValidFunction, args, NULL, true);
            buildExpr(ctx, test, tgt);
            return;
        }
        else
            type = alien->queryLogicalType();
    }

    CHqlBoundExpr bound;
    buildExpr(ctx, value, bound);
    ensureHasAddress(ctx, bound);

    OwnedHqlExpr address = getPointer(bound.expr);
    switch (type->getTypeCode())
    {
    case type_decimal:
        args.append(*createConstant(type->isSigned()));
        args.append(*getSizetConstant(type->getDigits()));
        args.append(*address.getLink());
        tgt.expr.setown(bindTranslatedFunctionCall(DecValidAtom, args));
        break;
    case type_real:
        args.append(*getSizetConstant(type->getSize()));
        args.append(*address.getLink());
        tgt.expr.setown(bindTranslatedFunctionCall(validRealAtom, args));
        break;
    default:
        tgt.expr.set(queryBoolExpr(true));
        break;
    }
}

IHqlExpression * HqlCppTranslator::getConstWuid(IHqlExpression * expr)
{
    SCMStringBuffer out;
    wu()->getWuid(out);
    OwnedHqlExpr wuid = createConstant(out.str());
    return ensureExprType(wuid, expr->queryType());
}

void HqlCppTranslator::doBuildAssignWuid(BuildCtx & ctx, const CHqlBoundTarget & target, IHqlExpression * expr)
{
    HqlExprArray args;
    OwnedHqlExpr call = bindFunctionCall(getWuidAtom, args);
    buildExprAssign(ctx, target, call);
}

void HqlCppTranslator::doBuildExprWuid(BuildCtx & ctx, IHqlExpression * expr, CHqlBoundExpr & tgt)
{
    HqlExprArray args;
    OwnedHqlExpr call = bindFunctionCall(getWuidAtom, args);
    buildTempExpr(ctx, call, tgt);
}

IHqlExpression * HqlCppTranslator::cvtGetEnvToCall(IHqlExpression * expr)
{
    IHqlExpression * dft = queryRealChild(expr, 1);
    HqlExprArray args;
    args.append(*LINK(expr->queryChild(0)));
    if (dft)
        args.append(*LINK(dft));
    else
        args.append(*createConstant(createStringValue((const char *)NULL, 0U)));
    return bindFunctionCall(getEnvAtom, args);
}

//---------------------------------------------------------------------------

void HqlCppTranslator::doBuildAssignToFromUnicode(BuildCtx & ctx, const CHqlBoundTarget & target, IHqlExpression * expr) 
{
    HqlExprArray args;
    if(!target.isFixedSize())
    {
        args.append(*LINK(expr->queryChild(0)));
        args.append(*foldHqlExpression(expr->queryChild(1)));
        OwnedHqlExpr call = bindFunctionCall((expr->getOperator() == no_fromunicode) ? unicode2CodepageXAtom : codepage2UnicodeXAtom, args);
        buildExprAssign(ctx, target, call);
    }
    else
    {
        args.append(*createTranslated(target.expr));
        args.append(*LINK(expr->queryChild(0)));
        args.append(*foldHqlExpression(expr->queryChild(1)));
        OwnedHqlExpr call = bindFunctionCall((expr->getOperator() == no_fromunicode) ? unicode2CodepageAtom : codepage2UnicodeAtom, args);
        buildStmt(ctx, call);
    }
}

void HqlCppTranslator::doBuildExprToFromUnicode(BuildCtx & ctx, IHqlExpression * expr, CHqlBoundExpr & tgt)
{
    HqlExprArray args;
    args.append(*LINK(expr->queryChild(0)));
    args.append(*foldHqlExpression(expr->queryChild(1)));
    OwnedHqlExpr call = bindFunctionCall((expr->getOperator() == no_fromunicode) ? unicode2CodepageXAtom : codepage2UnicodeXAtom, args);
    buildExpr(ctx, call, tgt);
}

//---------------------------------------------------------------------------

void HqlCppTranslator::doBuildExprKeyUnicode(BuildCtx & ctx, IHqlExpression * expr, CHqlBoundExpr & tgt)
{
    HqlExprArray args;
    args.append(*LINK(expr->queryChild(0)));
    args.append(*LINK(expr->queryChild(1)));
    args.append(*LINK(expr->queryChild(2)));
    OwnedHqlExpr call = bindFunctionCall(keyUnicodeStrengthXAtom, args);
    buildExpr(ctx, call, tgt);
}

//---------------------------------------------------------------------------

void HqlCppTranslator::doBuildAssignWhich(BuildCtx & ctx, const CHqlBoundTarget & target, IHqlExpression * expr)
{
    BuildCtx whichCtx(ctx);
    unsigned max = expr->numChildren();
    unsigned idx;
    bool invert = (expr->getOperator() == no_rejected);
    for (idx = 0; idx < max; idx++)
    {
        IHqlExpression * cur = expr->queryChild(idx);
        CHqlBoundExpr bound;
        if (invert)
        {
            OwnedHqlExpr inverse = getInverse(cur);
            buildExpr(whichCtx, inverse, bound);
        }
        else
            buildExpr(whichCtx, cur, bound);

        IHqlStmt * stmt = whichCtx.addFilter(bound.expr);
        OwnedHqlExpr value = createConstant(target.queryType()->castFrom(false, idx+1));
        assignBound(whichCtx, target, value);
        whichCtx.selectElse(stmt);
    }
    assignBound(whichCtx, target, queryZero());
}

//---------------------------------------------------------------------------

void HqlCppTranslator::assignBound(BuildCtx & ctx, const CHqlBoundTarget & lhs, IHqlExpression * rhs)
{
    CHqlBoundExpr bound;
    bound.expr.set(rhs);
    assign(ctx, lhs, bound);
}

void HqlCppTranslator::assignBoundToTemp(BuildCtx & ctx, IHqlExpression * lhs, IHqlExpression * rhs)
{
    CHqlBoundExpr bound;
    CHqlBoundTarget target;

    bound.expr.set(rhs);
    target.expr.set(lhs);
    assign(ctx, target, bound);
}

void HqlCppTranslator::assign(BuildCtx & ctx, const CHqlBoundTarget & target, CHqlBoundExpr & rhs)
{
    if (!target.isFixedSize())
    {
        assignCastUnknownLength(ctx, target, rhs);
        return;
    }

    IHqlExpression * lhs = target.expr;
    ITypeInfo * lType = lhs->queryType()->queryPromotedType();
    if (!isSameBasicType(lType, rhs.expr->queryType()->queryPromotedType()))
        assignAndCast(ctx, target, rhs);
    else
    {
        switch (lType->getTypeCode())
        {
            case type_decimal:
                if (isPushed(rhs))
                {
                    _ATOM funcName = lType->isSigned() ? DecPopDecimalAtom : DecPopUDecimalAtom;
                    HqlExprArray args;
                    args.append(*getPointer(lhs));
                    args.append(*getSizetConstant(lType->getSize()));
                    args.append(*getSizetConstant(lType->getPrecision()));
                    callProcedure(ctx, funcName, args);
                    return;
                }
                buildBlockCopy(ctx, lhs, rhs);
                return;
            case type_string:
                {
                    if (lType->getSize() == 1 && !options.peephole)
                    {
                        OwnedHqlExpr l1 = getFirstCharacter(lhs);
                        OwnedHqlExpr r1 = getFirstCharacter(rhs.expr);
                        ctx.addAssign(l1, r1);
                    }
                    else
                        buildBlockCopy(ctx, lhs, rhs);
                    break;
                }
                    //fall through...
            case type_unicode:
            case type_data:
            case type_qstring:
            case type_utf8:
                {
                    buildBlockCopy(ctx, lhs, rhs);
                    break;
                }
            case type_varstring:
                {
                    HqlExprArray args;
                    args.append(*getElementPointer(lhs));
                    args.append(*getElementPointer(rhs.expr));
                    
                    callProcedure(ctx, strcpyAtom, args);
                    break;
                }
            case type_varunicode:
                {
                    HqlExprArray args;
                    args.append(*getElementPointer(lhs));
                    args.append(*getElementPointer(rhs.expr));
                    
                    callProcedure(ctx, unicodeStrcpyAtom, args);
                    break;
                }
            case type_row:
                {
                    if (hasWrapperModifier(target.queryType()))
                    {
                        //I can't think of any situation where this isn't true....
                        assertex(hasLinkCountedModifier(rhs.expr));
                        StringBuffer assignText;
                        generateExprCpp(assignText, lhs).append(".set(");
                        generateExprCpp(assignText, rhs.expr).append(");");
                        ctx.addQuoted(assignText);
                        //Could generate the following instead
                        //ctx.addAssign(lhs, no_link(rhs.expr));
                        //And post-optimize to the above.
                    }
                    else
                        ctx.addAssign(lhs, rhs.expr);
                    break;
                }

            default:
                ctx.addAssign(lhs, rhs.expr);
                break;
        }
    }
}


void HqlCppTranslator::doStringTranslation(BuildCtx & ctx, ICharsetInfo * tgtset, ICharsetInfo * srcset, unsigned tgtlen, IHqlExpression * srclen, IHqlExpression * target, IHqlExpression * src)
{
    HqlExprArray args;
    ITranslationInfo * translator = queryDefaultTranslation(tgtset, srcset);
    if (translator)
    {
        _ATOM func = createIdentifierAtom(translator->queryRtlFunction());
        args.append(*getSizetConstant(tgtlen));
        args.append(*getElementPointer(target));
        args.append(*LINK(srclen));
        args.append(*getElementPointer(src));
        callProcedure(ctx, func, args);
    }
}



void HqlCppTranslator::assignSwapInt(BuildCtx & ctx, ITypeInfo * to, const CHqlBoundTarget & target, CHqlBoundExpr & pure)
{
    switch (pure.expr->getOperator())
    {
    case no_deref:
    case no_variable:
        break;
    default:
        {
            OwnedHqlExpr translated = pure.getTranslatedExpr();
            pure.clear();
            buildTempExpr(ctx, translated, pure);
            break;
        }
    }

    ITypeInfo * from = pure.expr->queryType();
    unsigned copySize = to->getSize();
    assertex(copySize == from->getSize());

    IHqlExpression * address = getRawAddress(pure.expr);
    switch (copySize)
    {
    case 1: 
        ctx.addAssign(target.expr, pure.expr);
        break;
    default:
        {
            HqlExprArray args;
            args.append(*address);

            OwnedHqlExpr call = bindTranslatedFunctionCall(reverseIntAtom[copySize][to->isSigned()], args);
            ctx.addAssign(target.expr, call);
            break;
        }
    }
}

void HqlCppTranslator::throwCannotCast(ITypeInfo * from, ITypeInfo * to)
{
    StringBuffer fromText, toText;
    getFriendlyTypeStr(from, fromText);
    getFriendlyTypeStr(to, toText);
    throwError2(HQLERR_CastXNotImplemented, fromText.str(), toText.str());
}


void HqlCppTranslator::assignAndCast(BuildCtx & ctx, const CHqlBoundTarget & target, CHqlBoundExpr & pure)
{
    if (!target.isFixedSize())
    {
        assignCastUnknownLength(ctx, target, pure);
        return;
    }

    ITypeInfo * to = target.queryType()->queryPromotedType();
    if ((pure.expr->getOperator() == no_constant) && options.foldConstantCast && 
        ((options.inlineStringThreshold == 0) || (to->getSize() <= options.inlineStringThreshold)))
    {
        OwnedHqlExpr cast = getCastExpr(to, pure.expr);
        if (cast)
        {
            assignBound(ctx, target, cast);
            return;
        }
    }

    //NB: Does not include variable length return types....
    ITypeInfo * from = (pure.expr->queryType()->queryPromotedType());
    type_t toType = to->getTypeCode();
    type_t fromType = from->getTypeCode();
    unsigned toSize = to->getSize();
    IHqlExpression * targetVar = target.expr;
    HqlExprArray args;
    assertex(targetVar);
    assertex(toSize != UNKNOWN_LENGTH);

    switch(toType)
    {
    case type_qstring:
        switch (fromType)
        {
        case type_qstring:
            {
                unsigned srcsize = from->getSize();
                if (!pure.length && (srcsize == toSize))
                {
                    //memcpy(tgt, src, srclen)
                    args.append(*getElementPointer(targetVar));
                    args.append(*getElementPointer(pure.expr));
                    args.append(*getSizetConstant(toSize));
                    callProcedure(ctx, memcpyAtom, args);
                }
                else
                {
                    args.append(*getSizetConstant(to->getStringLen()));
                    args.append(*getElementPointer(targetVar));
                    args.append(*getBoundLength(pure));
                    args.append(*getElementPointer(pure.expr));
                    callProcedure(ctx, qstrToQStrAtom, args);
                }
                break;
            }
        case type_data:
        case type_varstring:
        case type_string:
            {
                if(!queryDefaultTranslation(to->queryCharset(), from->queryCharset()))
                {
                    args.append(*getSizetConstant(to->getStringLen()));
                    args.append(*getElementPointer(targetVar));
                    args.append(*getBoundLength(pure));
                    args.append(*getElementPointer(pure.expr));
                    callProcedure(ctx, strToQStrAtom, args);
                    break;
                }
                //fall through
            }
        default:
            //Need to go via a temporary string.
            OwnedHqlExpr temp = pure.getTranslatedExpr();
            buildExprAssignViaString(ctx, target, temp, to->getStringLen());
            return;
        }
        break;

    case type_data:
    case type_string:
    case type_varstring:
        {
            unsigned srclen = from->getSize();
            ICharsetInfo * srcset = NULL;
            ICharsetInfo * tgtset = to->queryCharset();

            switch (fromType)
            {
                case type_data:
                case type_string:
                case type_varstring:
                    {
                        srcset = from->queryCharset();

                        OwnedHqlExpr boundLen = getBoundLength(pure);
                        if ((srcset == tgtset) || (toType == type_data) || (fromType == type_data))
                        {
                            bool doDefault = true;
                            if (boundLen->queryValue())
                            {
                                unsigned srclen = (unsigned)boundLen->queryValue()->getIntValue();
                                if (srclen >= toSize && toType != type_varstring)
                                {
                                    if (srclen > toSize)
                                        srclen = toSize;

                                    //memcpy(tgt, src, srclen)
                                    args.append(*getElementPointer(targetVar));
                                    args.append(*getElementPointer(pure.expr));
                                    args.append(*getSizetConstant(srclen));
                                    callProcedure(ctx, memcpyAtom, args);
                                    doDefault = false;
                                }
                            }

                            if (doDefault)
                            {
                                if (fromType == type_varstring)
                                {
                                    _ATOM func;
                                    switch (toType)
                                    {
                                    case type_varstring: func = vstr2VStrAtom; break;
                                    case type_string:    func = vstr2StrAtom; break;
                                    case type_data:      func = vstr2DataAtom; break;
                                    default: UNIMPLEMENTED;
                                    }

                                    if ((toSize < srclen) || (srclen==UNKNOWN_LENGTH) || (toType != type_varstring))
                                    {
                                        args.append(*getSizetConstant(toSize));
                                        args.append(*getElementPointer(targetVar));
                                        args.append(*getElementPointer(pure.expr));
                                        callProcedure(ctx, func, args);
                                    }
                                    else
                                    {
                                        //strcpy(tgt, src);
                                        args.append(*getElementPointer(targetVar));
                                        args.append(*getElementPointer(pure.expr));
                                        callProcedure(ctx, strcpyAtom, args);
                                    }
                                }
                                else
                                {
                                    _ATOM func;
                                    switch (toType)
                                    {
                                    case type_data:
                                        func = str2DataAtom;
                                        break;
                                    case type_varstring:
                                        func = str2VStrAtom;
                                        break;
                                    case type_string:
                                        func = (srcset->queryName() == ebcdicAtom) ? estr2EStrAtom : str2StrAtom;
                                        break;
                                    }
                                    args.append(*getSizetConstant(toSize));
                                    args.append(*getElementPointer(targetVar));
                                    args.append(*LINK(boundLen));
                                    args.append(*getElementPointer(pure.expr));
                                    callProcedure(ctx, func, args);
                                }
                            }
                        }
                        else
                        {
                            if ((from->getSize() == INFINITE_LENGTH) && !pure.length)
                                throwError(HQLERR_CastInfiniteString);
                            
                            IHqlExpression * srclen;
                            if (toType == type_varstring)
                            {
                                srclen = getSizetConstant(toSize);
                                args.append(*srclen);
                                args.append(*getElementPointer(targetVar));
                                args.append(*LINK(boundLen));
                                args.append(*getElementPointer(pure.expr));
                                callProcedure(ctx, estr2VStrAtom, args);
                            }
                            else
                                doStringTranslation(ctx, tgtset, srcset, toSize, boundLen, targetVar, pure.expr);

                        }
                    }
                    break;
                case type_qstring:
                    if (queryDefaultTranslation(tgtset, from->queryCharset()))
                    {
                        OwnedHqlExpr temp = pure.getTranslatedExpr();
                        buildExprAssignViaString(ctx, target, temp, to->getStringLen());
                    }
                    else
                    {
                        _ATOM func;
                        switch (toType)
                        {
                        case type_varstring: func = qstr2VStrAtom; break;
                        case type_string:    func = qstr2StrAtom; break;
                        case type_data:      func = qstr2DataAtom; break;
                        }

                        args.append(*getSizetConstant(toSize));
                        args.append(*getElementPointer(targetVar));
                        args.append(*getBoundLength(pure));
                        args.append(*getElementPointer(pure.expr));
                        callProcedure(ctx, func, args);
                    }
                    break;
                case type_unicode:
                case type_varunicode:
                    {
                        _ATOM func;
                        switch(toType)
                        {
                        case type_data:
                            func = (fromType == type_varunicode) ? vunicode2DataAtom : unicode2DataAtom;
                            break;
                        case type_string:
                            func = (fromType == type_varunicode) ? vunicode2CodepageAtom : unicode2CodepageAtom;
                            break;
                        case type_varstring:
                            func = (fromType == type_varunicode) ? vunicode2VCodepageAtom : unicode2VCodepageAtom;
                            break;
                        }
                        args.append(*getSizetConstant(toSize));
                        args.append(*getElementPointer(targetVar));
                        if(fromType != type_varunicode)
                            args.append(*getBoundLength(pure));
                        args.append(*getElementPointer(pure.expr));
                        if(toType != type_data)
                            args.append(*createConstant(to->queryCharset()->queryCodepageName()));
                        callProcedure(ctx, func, args);
                    }
                    break;
                case type_utf8:
                    {
                        _ATOM func;
                        switch(toType)
                        {
                        case type_data:
                            func = utf82DataAtom;
                            break;
                        case type_string:
                            func = utf82CodepageAtom;
                            break;
                        case type_varstring:
                            OwnedHqlExpr temp = pure.getTranslatedExpr();
                            buildExprAssignViaString(ctx, target, temp, to->getStringLen());
                            return;
                        }
                        args.append(*getSizetConstant(toSize));
                        args.append(*getElementPointer(targetVar));
                        args.append(*getBoundLength(pure));
                        args.append(*getElementPointer(pure.expr));
                        if(toType != type_data)
                            args.append(*createConstant(to->queryCharset()->queryCodepageName()));
                        callProcedure(ctx, func, args);
                    }
                    break;
                case type_swapint:
                    {
                        CHqlBoundExpr recast;
                        ITypeInfo * tempType = makeIntType(srclen, from->isSigned());
                        OwnedHqlExpr translated = createValue(no_implicitcast, tempType, pure.getTranslatedExpr());
                        buildExpr(ctx, translated, recast);
                        assignAndCast(ctx, target, recast);
                        return;
                    }
                case type_int:
                case type_packedint:
                    {
                        //l2an4(toSize, tgt, expr);
                        _ATOM funcName;
                        if (from->isSigned())
                        {
                            if (toType != type_varstring)
                                funcName = (srclen > 4 ? ls82anAtom : ls42anAtom);
                            else
                                funcName = (srclen > 4 ? ls82vnAtom : ls42vnAtom);
                        }
                        else
                        {
                            if (toType != type_varstring)
                                funcName = (srclen > 4 ? l82anAtom : l42anAtom);
                            else
                                funcName = (srclen > 4 ? l82vnAtom : l42vnAtom);
                        }

                        IHqlExpression * strlen = getSizetConstant(toSize);
                        args.append(*strlen);
                        args.append(*getElementPointer(targetVar));
                        args.append(*LINK(pure.expr));
                        callProcedure(ctx, funcName, args);
                        if (toType != type_data)
                        {
                            Owned<ICharsetInfo> charset = getCharset(asciiAtom);
                            doStringTranslation(ctx, tgtset, charset, toSize, strlen, targetVar, targetVar);
                        }
                        break;
                    }
                case type_void:
                    if (pure.expr->getOperator() != no_decimalstack)
                    {
                        throwCannotCast(from, to);
                        break;
                    }
                    //fall through
                case type_decimal:
                    {
                        ensurePushed(ctx, pure);
                        args.append(*getSizetConstant(toSize));
                        OwnedHqlExpr sp = getElementPointer(targetVar);
                        args.append(*ensureIndexable(sp));

                        _ATOM func;
                        switch (toType)
                        {
                        case type_string: func = DecPopStringAtom; break;
                        case type_data:   func = DecPopStringAtom; break;
                        case type_varstring: func = DecPopVStringAtom; break;
                        }
                        callProcedure(ctx, func, args);
                        break;
                    }
                case type_enumerated:
                    throwCannotCast(from, to);
                    break;
                case type_boolean:
                    {
                        _ATOM func = (toType == type_varstring) ? bool2VStrAtom : (toType == type_data) ? bool2DataAtom : bool2StrAtom;
                        args.append(*getSizetConstant(toSize));
                        args.append(*getElementPointer(targetVar));
                        args.append(*pure.expr.getLink());
                        callProcedure(ctx, func, args);
                        break;
                    }
                case type_real:
                    {
                        IHqlExpression * strlen = getSizetConstant(toSize);
                        args.append(*strlen);
                        args.append(*getElementPointer(targetVar));
                        args.append(*pure.expr.getLink());
                        _ATOM func = (toType == type_varstring) ? f2vnAtom : f2anAtom;
                        callProcedure(ctx, func, args);
                        if (toType != type_data)
                        {
                            Owned<ICharsetInfo> charset = getCharset(asciiAtom);
                            doStringTranslation(ctx, tgtset, charset, toSize, strlen, targetVar, targetVar);
                        }
                    }
                    break;
                default:
                    throwCannotCast(from, to);
                    break;
            }
        }
        break;

    case type_unicode:
    case type_varunicode:
        switch (fromType)
        {
        case type_unicode:
        case type_varunicode:
        case type_data:
        case type_string:
        case type_varstring:
        case type_utf8:
            {
                _ATOM func;
                switch(fromType)
                {
                case type_unicode:
                    func = (toType == type_varunicode) ? unicode2VUnicodeAtom : unicode2UnicodeAtom;
                    break;
                case type_varunicode:
                    func = (toType == type_varunicode) ? vunicode2VUnicodeAtom : vunicode2UnicodeAtom;
                    break;
                case type_data:
                    pure.expr.setown(createValue(no_implicitcast, makeReferenceModifier(makeStringType(from->getStringLen(), NULL)), LINK(pure.expr)));
                    func = (toType == type_varunicode) ? codepage2VUnicodeAtom : codepage2UnicodeAtom;
                    break;
                case type_string:
                    func = (toType == type_varunicode) ? codepage2VUnicodeAtom : codepage2UnicodeAtom;
                    break;
                case type_varstring:
                    func = (toType == type_varunicode) ? vcodepage2VUnicodeAtom : vcodepage2UnicodeAtom;
                    break;
                case type_utf8:
                    if (toType == type_varunicode)
                    {
                        OwnedHqlExpr temp = pure.getTranslatedExpr();
                        OwnedITypeInfo type = makeUnicodeType(to->getStringLen(), NULL);
                        buildExprAssignViaType(ctx, target, temp, type);
                        return;
                    }
                    func = utf82UnicodeAtom;
                    break;
                }
                args.append(*getSizetConstant(toSize/2));
                args.append(*getElementPointer(targetVar));
                if((fromType != type_varunicode) && (fromType != type_varstring))
                    args.append(*getBoundLength(pure));
                args.append(*getElementPointer(pure.expr));
                if((fromType == type_data) || (fromType == type_string) || (fromType == type_varstring))
                    args.append(*createConstant(from->queryCharset()->queryCodepageName()));
                callProcedure(ctx, func, args);
                break;
            }
        default:
            OwnedHqlExpr temp = pure.getTranslatedExpr();
            buildExprAssignViaString(ctx, target, temp, to->getStringLen());
            return;
        }
        break;

    case type_utf8:
        switch (fromType)
        {
        case type_unicode:
        case type_varunicode:
        case type_data:
        case type_string:
        case type_varstring:
        case type_utf8:
            {
                _ATOM func;
                switch(fromType)
                {
                case type_unicode:
                case type_varunicode:
                    func = unicodeToUtf8Atom;
                    break;
                case type_utf8:
                    func = utf8ToUtf8Atom;
                    break;
                case type_data:
                case type_string:
                case type_varstring:
                    func = codepageToUtf8Atom;
                    break;
                }
                args.append(*getSizetConstant(toSize/4));
                args.append(*getElementPointer(targetVar));
                args.append(*getBoundLength(pure));
                args.append(*getElementPointer(pure.expr));
                if((fromType == type_data) || (fromType == type_string) || (fromType == type_varstring))
                    args.append(*createConstant(from->queryCharset()->queryCodepageName()));
                callProcedure(ctx, func, args);
                break;
            }
        default:
            OwnedHqlExpr temp = pure.getTranslatedExpr();
            buildExprAssignViaString(ctx, target, temp, to->getStringLen());
            return;
        }
        break;

    case type_decimal:
        {
            CHqlBoundExpr cast;
            doBuildExprCast(ctx, to, pure, cast);

            ensurePushed(ctx, cast);
            _ATOM funcName = to->isSigned() ? DecPopDecimalAtom : DecPopUDecimalAtom;
            args.append(*getPointer(target.expr));
            args.append(*getSizetConstant(to->getSize()));
            args.append(*getSizetConstant(to->getPrecision()));
            callProcedure(ctx, funcName, args);
        }
        break;
    case type_swapint:
        {
            unsigned fromSize = from->getSize();
            if (fromType == type_int)
            {
                if (fromSize != toSize)
                {
                    Owned<ITypeInfo> tempType = makeIntType(toSize, from->isSigned());
                    pure.expr.setown(ensureExprType(pure.expr, tempType));
                }

                if (toSize != 1)
                {
                    assignSwapInt(ctx, to, target, pure);
                    return;
                }
            }
            CHqlBoundExpr cast;
            doBuildExprCast(ctx, to, pure, cast);
            ctx.addAssign(target.expr, cast.expr);
        }
        break;
    case type_int:
    case type_packedint:
        {
            unsigned fromSize = from->getSize();
            if ((fromType == type_swapint) && !((fromSize == 1) && (toSize == 1)))
            {
                if (fromSize != toSize)
                {
                    CHqlBoundExpr tempInt;
                    OwnedITypeInfo tempType = makeIntType(fromSize, from->isSigned());
                    doBuildCastViaTemp(ctx, tempType, pure, tempInt);
                    
                    CHqlBoundExpr cast;
                    doBuildExprCast(ctx, to, tempInt, cast);
                    ctx.addAssign(target.expr, cast.expr);
                }
                else
                    assignSwapInt(ctx, to, target, pure);
                return;
            }
        }
        //fall through
    case type_boolean:
    case type_real:
    case type_row:
    case type_pointer:
        {
            CHqlBoundExpr cast;
            doBuildExprCast(ctx, to, pure, cast);
            ctx.addAssign(target.expr, cast.expr);
        }
        break;

    default:
        throwCannotCast(from, to);
        break;
    }
}


void HqlCppTranslator::assignCastUnknownLength(BuildCtx & ctx, const CHqlBoundTarget & target, CHqlBoundExpr & pure)
{
    assertex(!target.isFixedSize());
    // must be dynamically allocated return type

    ITypeInfo * to = target.queryType();
    ITypeInfo * from = pure.expr->queryType();
    type_t toType = to->getTypeCode();
    type_t fromType = from->getTypeCode();

    IHqlExpression * codepageParam = 0;
    HqlExprArray args;
    _ATOM funcName = NULL;

//  assertex(target.length && target.pointer || to->getTypeCode() == type_varstring || to->getTypeCode() == type_varunicode);
    switch (toType)
    {
        case type_qstring:
            {
                switch (fromType)
                {
                case type_qstring:
                    funcName = qstrToQStrXAtom;
                    break;
                case type_string:
                case type_data:
                case type_varstring:
                    if(!queryDefaultTranslation(to->queryCharset(), from->queryCharset()))
                    {
                        funcName = strToQStrXAtom;
                        break;
                    }
                    //fall through
                default:
                    CHqlBoundExpr recast;
                    ITypeInfo * type = makeStringType(to->getStringLen(), NULL, NULL);
                    OwnedHqlExpr translated = createValue(no_implicitcast, type, pure.getTranslatedExpr());
                    buildExpr(ctx, translated, recast);
                    assignCastUnknownLength(ctx, target, recast);
                    return;
                }
                break;
            }

        case type_string:
        case type_data:
            {
                unsigned srclen = from->getSize();
                switch (fromType)
                {
                case type_data:
                case type_string:
                case type_varstring:
                    {
                        ICharsetInfo * srcset = from->queryCharset();
                        ICharsetInfo * tgtset = to->queryCharset();
                        
                        if (to->getTypeCode() == type_data)
                            funcName = str2DataXAtom;
                        else if ((srcset == tgtset) || (from->getTypeCode() == type_data))
                        {
                            funcName = str2StrXAtom;
                        }
                        else
                        {
                            if ((from->getSize() == INFINITE_LENGTH) && !pure.length)
                                throwError(HQLERR_CastInfiniteString);

                            ITranslationInfo * translator = queryDefaultTranslation(tgtset, srcset);
                            funcName = createIdentifierAtom(translator->queryVarRtlFunction());
                        }
                    }
                    break;
                case type_qstring:
                    if(!queryDefaultTranslation(from->queryCharset(), to->queryCharset()))
                    {
                        funcName = (toType == type_data) ? qstr2DataXAtom : qstr2StrXAtom;
                        break;
                    }
                    else
                    {
                        CHqlBoundExpr recast;
                        ITypeInfo * type = makeStringType(to->getStringLen(), NULL, NULL);
                        OwnedHqlExpr translated = createValue(no_implicitcast, type, pure.getTranslatedExpr());
                        buildExpr(ctx, translated, recast);
                        assignCastUnknownLength(ctx, target, recast);
                        return;
                    }
                case type_unicode:
                    {
                        if(toType == type_data)
                            funcName = unicode2DataXAtom;
                        else
                        {
                            funcName = unicode2CodepageXAtom;
                            codepageParam = createConstant(to->queryCharset()->queryCodepageName());
                        }
                        break;
                    }
                case type_varunicode:
                    {
                        if(toType == type_data)
                            funcName = vunicode2DataXAtom;
                        else
                        {
                            funcName = vunicode2CodepageXAtom;
                            codepageParam = createConstant(to->queryCharset()->queryCodepageName());
                        }
                        break;
                    }
                case type_utf8:
                    {
                        if(toType == type_data)
                            funcName = utf82DataXAtom;
                        else
                        {
                            funcName = utf82CodepageXAtom;
                            codepageParam = createConstant(to->queryCharset()->queryCodepageName());
                        }
                        break;
                    }
                case type_swapint:
                    {
                        CHqlBoundExpr recast;
                        ITypeInfo * type = makeIntType(from->getSize(), from->isSigned());
                        OwnedHqlExpr translated = createValue(no_implicitcast, type, pure.getTranslatedExpr());
                        buildExpr(ctx, translated, recast);
                        assignCastUnknownLength(ctx, target, recast);
                        return;
                    }
                case type_int:
                case type_real:
                case type_boolean:
                case type_packedint:
                    {
                        Owned<ICharsetInfo> asciiCharset = getCharset(asciiAtom);
                        if (to->queryCharset() != asciiCharset)
                        {
                            //This should really be handled by the call processing.
                            CHqlBoundExpr recast;
                            ITypeInfo * type = makeStringType(to->getStringLen(), NULL, NULL);
                            OwnedHqlExpr translated = createValue(no_implicitcast, type, pure.getTranslatedExpr());
                            buildExpr(ctx, translated, recast);
                            assignCastUnknownLength(ctx, target, recast);
                            return;
                        }
                        if (fromType == type_real)
                            funcName = f2axAtom;
                        else if (fromType == type_boolean)
                            funcName = bool2StrXAtom;
                        else if (from->isSigned())
                            funcName = (srclen > 4 ? ls82axAtom : ls42axAtom);
                        else
                            funcName = (srclen > 4 ? l82axAtom : l42axAtom);
                        args.append(*pure.getTranslatedExpr());

                        OwnedHqlExpr call = bindFunctionCall(funcName, args);
                        buildExprAssign(ctx, target, call);
                        return;
                    }
                case type_void:
                    if (pure.expr->getOperator() != no_decimalstack)
                    {
                        throwCannotCast(from, to);
                        break;
                    }
                    //fall through
                case type_decimal:
                    {
                        ensurePushed(ctx, pure);
                        OwnedHqlExpr call = bindFunctionCall(DecPopStringXAtom, args);
                        buildExprAssign(ctx, target, call);
                        return;
                    }
                default:
                    assertex(!"Unknown copy source type");
                    return;
                }
                break;
            }
        case type_varstring:
            {
                unsigned srclen = from->getSize();
                switch (from->getTypeCode())
                {
                case type_data:
                case type_string:
                case type_varstring:
                    {
                        ICharsetInfo * srcset = from->queryCharset();
                        ICharsetInfo * tgtset = to->queryCharset();
                        
                        if ((srcset == tgtset) || (to->getTypeCode() == type_data) || (from->getTypeCode() == type_data))
                        {
                            funcName = str2VStrXAtom;
                        }
                        else
                        {
                            funcName = estr2VStrXAtom;

                        }
                    }
                    break;
                case type_unicode:
                    {
                        funcName = unicode2VCodepageXAtom;
                        codepageParam = createConstant(to->queryCharset()->queryCodepageName());
                    }
                    break;
                case type_varunicode:
                    {
                        funcName = vunicode2VCodepageXAtom;
                        codepageParam = createConstant(to->queryCharset()->queryCodepageName());
                    }
                    break;
                case type_qstring:
                case type_utf8:
                    {
                        CHqlBoundExpr recast;
                        ITypeInfo * type = makeStringType(from->getStringLen(), NULL, NULL);
                        OwnedHqlExpr translated = createValue(no_implicitcast, type, pure.getTranslatedExpr());
                        buildExpr(ctx, translated, recast);
                        assignCastUnknownLength(ctx, target, recast);
                        return;
                    }
                case type_swapint:
                    {
                        CHqlBoundExpr recast;
                        ITypeInfo * type = makeIntType(from->getSize(), from->isSigned());
                        OwnedHqlExpr translated = createValue(no_implicitcast, type, pure.getTranslatedExpr());
                        buildExpr(ctx, translated, recast);
                        assignCastUnknownLength(ctx, target, recast);
                        return;
                    }
                case type_int:
                case type_packedint:
                    {
                        //l2an4(tgtlen, tgt, expr);
                        if (from->isSigned())
                            funcName = (srclen > 4 ? ls82vxAtom : ls42vxAtom);
                        else
                            funcName = (srclen > 4 ? l82vxAtom : l42vxAtom);
                        break;
                    }
                case type_boolean:
                    {
                        funcName = bool2VStrXAtom;
                        break;
                    }
                case type_real:
                    {
                        funcName = f2vxAtom;;
                        break;
                    }
                case type_void:
                    if (pure.expr->getOperator() != no_decimalstack)
                    {
                        throwCannotCast(from, to);
                        break;
                    }
                    //fall through
                case type_decimal:
                    {
                        ensurePushed(ctx, pure);
                        OwnedHqlExpr call = bindFunctionCall(DecPopVStringXAtom, args);
                        buildExprAssign(ctx, target, call);
                        return;
                    }
                default:
                    assertex(!"Unknown copy source type");
                    return;
                }
                break;
            }

        case type_unicode:
            {
                switch (fromType)
                {
                case type_unicode:
                    funcName = unicode2UnicodeXAtom;
                    break;
                case type_varunicode:
                    funcName = vunicode2UnicodeXAtom;
                    break;
                case type_utf8:
                    funcName = utf82UnicodeXAtom;
                    break;
                case type_data:
                    funcName = codepage2UnicodeXAtom;
                    codepageParam = createConstant(from->queryCharset()->queryCodepageName());
                    pure.expr.setown(createValue(no_implicitcast, makeStringType(from->getStringLen(), NULL, NULL), LINK(pure.expr)));
                    break;
                case type_string:
                    funcName = codepage2UnicodeXAtom;
                    codepageParam = createConstant(from->queryCharset()->queryCodepageName());
                    pure.expr.setown(createValue(no_typetransfer, makeStringType(from->getStringLen(), NULL, NULL), LINK(pure.expr)));
                    break;
                case type_varstring:
                    funcName = vcodepage2UnicodeXAtom;
                    codepageParam = createConstant(from->queryCharset()->queryCodepageName());
                    pure.expr.setown(createValue(no_typetransfer, makeVarStringType(from->getStringLen(), NULL, NULL), LINK(pure.expr)));
                    break;
                default:
                    CHqlBoundExpr recast;
                    ITypeInfo * type = makeStringType(to->getStringLen(), NULL, NULL);
                    OwnedHqlExpr translated = createValue(no_implicitcast, type, pure.getTranslatedExpr());
                    buildExpr(ctx, translated, recast);
                    assignCastUnknownLength(ctx, target, recast);
                    return;
                }
                break;
            }

        case type_varunicode:
            {
                switch (fromType)
                {
                case type_unicode:
                case type_utf8:             // go via unicode
                    funcName = unicode2VUnicodeXAtom;
                    break;
                case type_varunicode:
                    funcName = vunicode2VUnicodeXAtom;
                    break;
                case type_string:
                case type_data:
                    funcName = codepage2VUnicodeXAtom;
                    codepageParam = createConstant(from->queryCharset()->queryCodepageName());
                    pure.expr.setown(createValue(no_typetransfer, makeStringType(from->getStringLen(), NULL, NULL), LINK(pure.expr)));
                    break;
                case type_varstring:
                    funcName = vcodepage2VUnicodeXAtom;
                    codepageParam = createConstant(from->queryCharset()->queryCodepageName());
                    pure.expr.setown(createValue(no_typetransfer, makeVarStringType(from->getStringLen(), NULL, NULL), LINK(pure.expr)));
                    break;
                default:
                    CHqlBoundExpr recast;
                    ITypeInfo * type = makeStringType(to->getStringLen(), NULL, NULL);
                    OwnedHqlExpr translated = createValue(no_implicitcast, type, pure.getTranslatedExpr());
                    buildExpr(ctx, translated, recast);
                    assignCastUnknownLength(ctx, target, recast);
                    return;
                }
                break;
            }
        case type_utf8:
            {
                switch (fromType)
                {
                case type_unicode:
                case type_varunicode:
                    funcName = unicodeToUtf8XAtom;
                    break;
                case type_utf8:
                    funcName = utf8ToUtf8XAtom;
                    break;
                case type_string:
                case type_data:
                case type_varstring:
                    funcName = codepageToUtf8XAtom;
                    codepageParam = createConstant(from->queryCharset()->queryCodepageName());
                    pure.expr.setown(createValue(no_typetransfer, makeStringType(from->getStringLen(), NULL, NULL), LINK(pure.expr)));
                    break;
                default:
                    CHqlBoundExpr recast;
                    ITypeInfo * type = makeStringType(to->getStringLen(), NULL, NULL);
                    OwnedHqlExpr translated = createValue(no_implicitcast, type, pure.getTranslatedExpr());
                    buildExpr(ctx, translated, recast);
                    assignCastUnknownLength(ctx, target, recast);
                    return;
                }
                break;
            }

        case type_set:
            if (isSameBasicType(to->queryChildType(), from->queryChildType()))
            {
                if (!target.isAll)
                {
                    //Ugly.  Create a dummy isAll field to assign to..
                    assertex(!pure.isAll || matchesBoolean(pure.isAll, false));
                    CHqlBoundTarget tempTarget;
                    tempTarget.set(target);
                    tempTarget.isAll.setown(ctx.getTempDeclare(queryBoolType(), NULL));
                    assignCastUnknownLength(ctx, tempTarget, pure);
                    return;
                }
                funcName = set2SetXAtom;
            }
            else
            {
                OwnedHqlExpr values = pure.getTranslatedExpr();
                buildSetAssignViaBuilder(ctx, target, values);
                return;
            }
            break;

        case type_table:
        case type_groupedtable:
            {
                OwnedHqlExpr src = pure.getTranslatedExpr();
                buildDatasetAssign(ctx, target, src);
                return;
            }

        default:
            assertex(!"Unexpected target type for variable length");
            break;
    }


    args.append(*pure.getTranslatedExpr());
    if(codepageParam)
        args.append(*codepageParam);
    OwnedHqlExpr call = bindFunctionCall(funcName, args);
    buildExprAssign(ctx, target, call);
}


void HqlCppTranslator::expandFunctions(bool expandInline)
{
    if (expandInline)
    {
        BuildCtx ctx(*code, prototypeAtom);
        ForEachItemIn(idx, code->helpers)
        {
            IHqlExpression & cur = (IHqlExpression &)code->helpers.item(idx);
            StringBuffer init;
            if (getProperty(cur.queryChild(0), initfunctionAtom, init))
            {
                StringBuffer initproto("extern \"C\" void SERVICE_API ");
                initproto.append(init).append("(const char *);");
                ctx.addQuoted(initproto);
            }
            expandFunctionPrototype(ctx, &cur, false);
        }
    }
    else
    {
        CIArray includes;

        BuildCtx ctx(*code, includeAtom);
        ForEachItemIn(idx, code->helpers)
        {
            //StringBuffer include;
            //IHqlExpression & cur = (IHqlExpression &)code->helpers.item(idx);
            // getLibraryName(cur, include);
            //MORE!! Get the include name...
        }
    }
}


void HqlCppTranslator::bindAndPush(BuildCtx & ctx, IHqlExpression * value)
{
    CHqlBoundExpr bound;
    buildExpr(ctx, value, bound);
    ensurePushed(ctx, bound);
}

bool HqlCppTranslator::ensurePushed(BuildCtx & ctx, const CHqlBoundExpr & pure)
{
    if (!isPushed(pure))
    {
        //Temporary solution - create a critical block whenever the decimals are used.
        OwnedHqlExpr marker = createAttribute(decimalAtom);
        if (!ctx.queryMatchExpr(marker))
        {
            //If a group with no {} is added, we might get a name clash => make it unique
            StringBuffer s;
            getUniqueId(s.append("BcdCriticalBlock bcd")).append(";");
            ctx.addQuoted(s);
            ctx.associateExpr(marker, NULL);
        }

        ITypeInfo * type = pure.expr->queryType();

        HqlExprArray args;
        _ATOM funcName = NULL;
        switch (type->getTypeCode())
        {
            case type_data:
            case type_string:
            case type_varstring:
                funcName = DecPushStringAtom;
                if (type->queryCharset()->queryName() == ebcdicAtom)
                {
                    CHqlBoundExpr temp;
                    OwnedHqlExpr translated = pure.getTranslatedExpr();
                    OwnedHqlExpr cast = createValue(no_cast, getAsciiType(type), translated.getClear());
                    buildExpr(ctx, cast, temp);
                    args.append(*getBoundLength(temp));
                    args.append(*getElementPointer(temp.expr));
                }
                else
                {
                    args.append(*getBoundLength(pure));
                    args.append(*getElementPointer(pure.expr));
                }
                break;
            case type_qstring:
                funcName = DecPushQStringAtom;
                args.append(*getBoundLength(pure));
                args.append(*getElementPointer(pure.expr));
                break;
            case type_unicode:
            case type_varunicode:
                funcName = DecPushUnicodeAtom;
                args.append(*getBoundLength(pure));
                args.append(*getElementPointer(pure.expr));
                break;
            case type_utf8:
                funcName = DecPushUtf8Atom;
                args.append(*getBoundLength(pure));
                args.append(*getElementPointer(pure.expr));
                break;
            case type_decimal:
                funcName = type->isSigned() ? DecPushDecimalAtom : DecPushUDecimalAtom;
                args.append(*getPointer(pure.expr));
                args.append(*getSizetConstant(type->getSize()));
                args.append(*getSizetConstant(type->getPrecision()));
                break;
            case type_swapint:
                {
                    CHqlBoundExpr copyPure;
                    copyPure.set(pure);
                    //cast via intermediate int.
                    OwnedITypeInfo tempType = makeIntType(type->getSize(), type->isSigned());
                    CHqlBoundExpr boundCast;
                    doBuildExprCast(ctx, tempType, copyPure, boundCast);
                    funcName = type->isSigned() ? DecPushInt64Atom : DecPushUInt64Atom;
                    args.append(*boundCast.expr.getLink());
                    break;
                }
                //fall through
            case type_int:
            case type_packedint:
                //more signed/unsigned and optimize the length...
                funcName = type->isSigned() ? DecPushInt64Atom : DecPushUInt64Atom;
                args.append(*pure.expr.getLink());
                break;
            case type_enumerated:
                throwError1(HQLERR_CastXNotImplemented, "map=>decimal");
                break;
            case type_boolean:
                funcName = DecPushLongAtom;
                args.append(*pure.expr.getLink());
                break;
            case type_real:
                funcName = DecPushRealAtom;
                args.append(*pure.expr.getLink());
                break;
            default:
                throwError1(HQLERR_CastXNotImplemented, "unknown=>varstring");
                break;
        }
        if (funcName)
            callProcedure(ctx, funcName, args);
        return true;
    }
    return false;
}

static StringBuffer & appendCapital(StringBuffer & s, StringBuffer & _name)
{
    const char * name = _name.str();
    if (name && name[0])
    {
        s.append((char)toupper(*name));
        s.append(name+1);
    }
    return s;
}

void HqlCppTranslator::expandFunctionReturnType(StringBuffer & prefix, StringBuffer & params, ITypeInfo * retType, IHqlExpression * attrs)
{
    type_t tc = retType->getTypeCode();
    switch (tc)
    {
    case type_varstring:
    case type_varunicode:
        if (retType->getSize() == UNKNOWN_LENGTH)
        {
            generateTypeCpp(prefix, retType, NULL);
            break;
        }
        //fall through
    case type_qstring:
    case type_string:
    case type_data:
    case type_unicode:
    case type_utf8:
        {
            OwnedITypeInfo ptrType = makeReferenceModifier(LINK(retType));
            prefix.append("void");
            if (retType->getSize() == UNKNOWN_LENGTH)
            {
                if (retType->getTypeCode() != type_varstring)
                    params.append("size32_t & __lenResult,");

    //          if (hasConstModifier(retType))
    //              params.append("const ");
                generateTypeCpp(params, retType, NULL);
                params.append(" & __result");
            }
            else
            {
                generateTypeCpp(params, ptrType, "__result");
            }
            break;
        }
    case type_transform:
        prefix.append("size32_t");
        params.append("ARowBuilder & __self");
        break;
    case type_table:
    case type_groupedtable:
        if (hasStreamedModifier(retType))
        {
            prefix.append("IRowStream *");
            params.append("IEngineRowAllocator * _resultAllocator");
        }
        else if (hasLinkCountedModifier(retType))
        {
            prefix.append("void");
            params.append("size32_t & __countResult,");
    //      if (hasConstModifier(retType))
    //          params.append("const ");
            params.append("byte * * & __result");
            if (hasNonNullRecord(retType) && getBoolProperty(attrs, allocatorAtom, true))
                params.append(", IEngineRowAllocator * _resultAllocator");
        }
        else
        {
            prefix.append("void");
            params.append("size32_t & __lenResult,");
    //      if (hasConstModifier(retType))
    //          params.append("const ");
            params.append("void * & __result");
        }
        break;
    case type_set:
        {
            prefix.append("void");
            if (!getBoolProperty(attrs, oldSetFormatAtom))
            {
                params.append("bool & __isAllResult,");
                params.append("size32_t & __lenResult,");
            }
            else
                params.append("unsigned & __numResult,");

//          if (hasConstModifier(retType))
//              params.append("const ");
            params.append("void * & __result");
            break;
        }
    case type_row:
        prefix.append("void");
        params.append("byte * __result");
        break;
    default:
        generateTypeCpp(prefix, retType, NULL);
        break;
    }
}


bool HqlCppTranslator::expandFunctionPrototype(StringBuffer & s, IHqlExpression * funcdef, bool force)
{
    IHqlExpression *body = funcdef->queryChild(0);
    IHqlExpression *formals = funcdef->queryChild(1);

    if (!force && (body->hasProperty(includeAtom) || body->hasProperty(ctxmethodAtom) || body->hasProperty(gctxmethodAtom) || body->hasProperty(methodAtom) || body->hasProperty(sysAtom) || body->hasProperty(omethodAtom) ))
        return false;

    enum { ServiceApi, RtlApi, BcdApi, CApi, LocalApi } api = ServiceApi;
    if (body->hasProperty(eclrtlAtom))
        api = RtlApi;
    else if (body->hasProperty(bcdAtom))
        api = BcdApi;
    else if (body->hasProperty(cAtom))
        api = CApi;
    else if (body->hasProperty(localAtom))
        api = LocalApi;

    s.append("extern ");
    if ((api == ServiceApi) || api == CApi)
        s.append("\"C\" ");

    switch (api)
    {
    case ServiceApi: s.append(" SERVICE_API"); break;
    case RtlApi:     s.append(" RTL_API"); break;
    case BcdApi:     s.append(" BCD_API"); break;
    }
    s.append(" ");

    StringBuffer returnParameters;
    ITypeInfo * retType = funcdef->queryType()->queryChildType();
    expandFunctionReturnType(s, returnParameters, retType, body);

    switch (api)
    {
    case CApi:
        switch (options.targetCompiler)
        {
        case Vs6CppCompiler:
            s.append(" _cdecl");
            break;
        }
        break;
    case BcdApi:
        switch (options.targetCompiler)
        {
        case Vs6CppCompiler:
            s.append(" __fastcall");
            break;
        }
        break;
    }
    s.append(" ");

    getProperty(body, entrypointAtom, s);
    bool firstParam = true;
    if (!body->hasProperty(entrypointAtom))
    {
        s.append("/*").append(body->queryName()).append("*/");
    }
    s.append('(');
    if (body->hasProperty(contextAtom))
    {
        s.append("ICodeContext * ctx");
        firstParam = false;
    }
    else if (body->hasProperty(globalContextAtom) )
    {
        s.append("IGlobalCodeContext * gctx");
        firstParam = false;
    }
    else if (body->hasProperty(userMatchFunctionAtom))
    {
        s.append("IMatchWalker * results");
        firstParam = false;
    }

    if (returnParameters.length())
    {
        if (!firstParam)
            s.append(',');
        s.append(returnParameters);
        firstParam = false;
    }

    ForEachChild(i, formals)
    {
        if (!firstParam)
            s.append(',');
        else
            firstParam = false;

        IHqlExpression * param = formals->queryChild(i);
        ITypeInfo *paramType = param->queryType();

        
        //Case is significant if these parameters are use for BEGINC++ sections
        _ATOM paramName = param->queryName();
        StringBuffer paramNameText;
        paramNameText.append(paramName).toLowerCase();

        bool isOut = false;
        bool isConst = false;
        unsigned maxAttr = param->numChildren();
        unsigned attrIdx;
        for (attrIdx = 0; attrIdx < maxAttr; attrIdx++)
        {
            IHqlExpression * attr = param->queryChild(attrIdx);
            if (attr->isAttribute())
            {
                if (attr->queryName() == constAtom)
                    isConst = true;
                else if (attr->queryName() == outAtom)
                    isOut = true;
            }
        }

        switch (paramType->getTypeCode())
        {
        case type_string:
        case type_qstring:
        case type_data:
        case type_unicode:
        case type_utf8:
        case type_table:
        case type_groupedtable:
            if (paramType->getSize() == UNKNOWN_LENGTH)
            {
                s.append("size32_t");
                if (isOut)
                    s.append(" &");
                if (paramName)
                    appendCapital(s.append(" len"), paramNameText);
                s.append(",");
            }
            break;
        case type_set:
            if (!queryProperty(paramType, oldSetFormatAtom))
            {
                s.append("bool");
                if (paramName)
                    appendCapital(s.append(" isAll"), paramNameText);
                s.append(",size32_t ");
                if(paramName)
                    appendCapital(s.append(" len"), paramNameText);
            }
            else
            {
                s.append("unsigned");
                if(paramName)
                    appendCapital(s.append(" num"), paramNameText);
            }
            s.append(",");
            break;
        case type_row:
            isConst = true;
            break;
        } 
        
        bool nameappended = false;
        switch (paramType->getTypeCode())
        {
        case type_record:
            s.append("IOutputMetaData * ");
            break;
        case type_table:
        case type_groupedtable:
            if (isConst)
                s.append("const ");
            s.append("void *");
            if (isOut)
                s.append(" &");
            break;
        case type_set:
            if (!queryProperty(paramType, oldSetFormatAtom))
            {
                if (isConst)
                    s.append("const ");
                s.append("void *");
                if (isOut)
                    s.append(" &");
                break;
            }
            else
            {
                ITypeInfo* childType = paramType->queryChildType();
                if(!childType)
                    break;
                if(isStringType(childType)) {
                    // process stringn and varstringn specially.
                    if(childType->getSize() > 0) {
                        s.append("char ");
                        if(paramName) {
                            s.append(paramNameText);
                            nameappended = true;
                        }
                        s.append("[][");
                        unsigned setlen = childType->getSize();
                        s.append(setlen);
                        s.append("]");
                    }
                    // Process string and varstring specially
                    else {
                        s.append("char *");
                        if(paramName) {
                            s.append(paramNameText);
                            nameappended = true;
                        }
                        s.append("[]");
                    }
                }
                else
                {
                    OwnedITypeInfo pointerType = makePointerType(LINK(childType));
                    generateTypeCpp(s, pointerType, NULL);
                }
                break;
            }
            // Other set types just fall through and will be treated like other types.
        case type_qstring: case type_string: case type_varstring: case type_data:
        default:
            {
                if (isConst)
                    s.append("const ");
                Owned<ITypeInfo> argType = LINK(paramType);
                if (argType->getTypeCode() == type_function)
                    argType.setown(makePointerType(LINK(argType)));
                if (isTypePassedByAddress(argType))
                    argType.setown(makeReferenceModifier(LINK(argType)));
                generateTypeCpp(s, argType, paramName->str());
                nameappended = true;
                if (isOut)
                    s.append(" &");
                break;
            }
        }
        if (paramName && !nameappended)
            s.append(" ").append(paramNameText);
    }
    s.append(")");
    return true;
}

void HqlCppTranslator::expandFunctionPrototype(BuildCtx & ctx, IHqlExpression * funcdef, bool force)
{
    StringBuffer s;
    if (expandFunctionPrototype(s, funcdef, force))
    {
        s.append(";");
        ctx.addQuoted(s);
    }
}

//Replace no_param with whatever they will have been bound to
static IHqlExpression * replaceInlineParameters(IHqlExpression * funcdef, IHqlExpression * expr)
{
    IHqlExpression * body = funcdef->queryChild(0);
    assertex(!body->hasProperty(oldSetFormatAtom));
    IHqlExpression * formals = funcdef->queryChild(1);

    HqlMapTransformer simpleTransformer;
    StringBuffer paramNameText, temp;
    ForEachChild(i, formals)
    {
        IHqlExpression * param = formals->queryChild(i);
        ITypeInfo *paramType = param->queryType();
        CHqlBoundExpr bound;
        
        //Case is significant if these parameters are use for BEGINC++ sections
        _ATOM paramName = param->queryName();
        paramNameText.clear().append(paramName).toLowerCase();

        Linked<ITypeInfo> type = paramType;
        switch (paramType->getTypeCode())
        {
        case type_set:
            {
                appendCapital(temp.clear().append("isAll"), paramNameText);
                bound.isAll.setown(createVariable(temp.str(), makeBoolType()));
            }
            //fall through
        case type_string:
        case type_qstring:
        case type_data:
        case type_unicode:
        case type_utf8:
        case type_table:
        case type_groupedtable:
            if (paramType->getSize() == UNKNOWN_LENGTH)
            {
                appendCapital(temp.clear().append("len"), paramNameText);
                bound.length.setown(createVariable(temp.str(), LINK(sizetType)));
            }
            type.setown(makeReferenceModifier(LINK(type)));
            break;
        } 
        bound.expr.setown(createVariable(paramNameText.str(), LINK(type)));
        OwnedHqlExpr replacement = bound.getTranslatedExpr();
        simpleTransformer.setMapping(param, replacement);
    }

    return simpleTransformer.transformRoot(expr);
}

void HqlCppTranslator::doBuildUserFunctionReturn(BuildCtx & ctx, ITypeInfo * type, IHqlExpression * value)
{
    if (!options.spotCSE)
    {
        doBuildFunctionReturn(ctx, type, value);
        return;
    }

    switch (value->getOperator())
    {
    case no_if:
        if (false)///disable for the moment - look at changes in klogermann11 to see why, some v.good, some bad.
        {
            //optimize the way that cses are spotted to minimise unnecessary calculations
            OwnedHqlExpr branches = createComma(LINK(value->queryChild(1)), LINK(value->queryChild(2)));
            OwnedHqlExpr cond = LINK(value->queryChild(0));
            spotScalarCSE(cond, branches, NULL, NULL);
            BuildCtx subctx(ctx);
            IHqlStmt * stmt = buildFilterViaExpr(subctx, cond);
            doBuildUserFunctionReturn(subctx, type, branches->queryChild(0));
            subctx.selectElse(stmt);
            doBuildUserFunctionReturn(subctx, type, branches->queryChild(1));
            break;
        }
    default:
        {
            OwnedHqlExpr optimized = spotScalarCSE(value);
            if (value->isAction())
                buildStmt(ctx, value);
            else
                doBuildFunctionReturn(ctx, type, optimized);
            break;
        }
    }
}


void HqlCppTranslator::buildFunctionDefinition(IHqlExpression * externalDef, IHqlExpression * funcdef)
{
    IHqlExpression *body = funcdef->queryChild(0);
    ITypeInfo * returnType = funcdef->queryType()->queryChildType();
    if (body->getOperator() == no_outofline)
        body = body->queryChild(0);

    StringBuffer s;
    BuildCtx funcctx(*code, helperAtom);
    if (options.spanMultipleCpp)
    {
        const bool inChildActivity = true;  // assume the worse
        OwnedHqlExpr pass = getSizetConstant(cppIndexNextActivity(inChildActivity));
        funcctx.addGroupPass(pass);
    }
    expandFunctionPrototype(s, externalDef, false);

    if (body->getOperator() == no_cppbody)
    {
        if (!allowEmbeddedCpp())
            throwError(HQLERR_EmbeddedCppNotAllowed);

        IHqlExpression * location = queryLocation(body);
        const char * locationFilename = location ? location->querySourcePath()->str() : NULL;
        unsigned startLine = location ? location->getStartLine() : 0;
        IHqlExpression * cppBody = body->queryChild(0);
        if (cppBody->getOperator() == no_record)
            cppBody = body->queryChild(1);

        StringBuffer text;
        cppBody->queryValue()->getStringValue(text);
        //remove /r so we don't end up with mixed format end of lines.
        text.setLength(cleanupEmbeddedCpp(text.length(), (char*)text.str()));

        const char * start = text.str();
        loop
        {
            char next = *start;
            if (next == '\n')
                startLine++;
            else if (next != '\r')
                break;
            start++;
        }

        const char * body = start;
        const char * cppSeparatorText = "#body";
        const char * separator = strstr(body, cppSeparatorText);
        if (separator)
        {
            text.setCharAt(separator-text.str(), 0);
            if (location)
                funcctx.addLine(locationFilename, startLine);
            funcctx.addQuoted(body);
            if (location)
                funcctx.addLine();

            body = separator + strlen(cppSeparatorText);
            if (*body == '\r') body++;
            if (*body == '\n') body++;
            startLine += memcount(body-start, start, '\n');
        }

        funcctx.addQuotedCompound(s);
        if (location)
            funcctx.addLine(locationFilename, startLine);
        funcctx.addQuoted(body);
        if (location)
            funcctx.addLine();
    }
    else
    {
        funcctx.addQuotedCompound(s);
        OwnedHqlExpr newBody = replaceInlineParameters(funcdef, body);
        newBody.setown(foldHqlExpression(newBody));
        doBuildUserFunctionReturn(funcctx, returnType, newBody);
    }
}

//---------------------------------------------------------------------------

void HqlCppTranslator::doBuildPureSubExpr(BuildCtx & ctx, IHqlExpression * expr, CHqlBoundExpr & tgt)
{
    unsigned max = expr->numChildren();
    if (max == 0)
        tgt.expr.set(expr);
    else
    {
        HqlExprArray args;
        unsigned idx = 0;
        CHqlBoundExpr bound;
        for (idx = 0; idx < max; idx++)
        {
            buildExpr(ctx, expr->queryChild(idx), bound);
            args.append(*bound.expr.getClear());
        }

        tgt.expr.setown(expr->clone(args));
    }
}


//---------------------------------------------------------------------------

IHqlExpression * HqlCppTranslator::getListLength(BuildCtx & ctx, IHqlExpression * expr)
{
    CHqlBoundExpr bound;
    buildExpr(ctx, expr, bound);
    return getBoundLength(bound);
}

IHqlExpression * HqlCppTranslator::getBoundCount(const CHqlBoundExpr & bound)
{
    if (bound.count)
        return bound.count.getLink();

    ITypeInfo * type = bound.expr->queryType();
    switch (type->getTypeCode())
    {
    case type_array:
        {
            if (bound.length)
                return convertBetweenCountAndSize(bound, true);
            unsigned size = type->getSize();
            if (size != UNKNOWN_LENGTH)
                return getSizetConstant(size / type->queryChildType()->getSize());
            UNIMPLEMENTED;
        }
    case type_table:
    case type_groupedtable:
    case type_set:
        if (bound.length)
            return convertBetweenCountAndSize(bound, true);
        UNIMPLEMENTED;
    default:
        UNIMPLEMENTED;
    }
}

IHqlExpression * HqlCppTranslator::getBoundLength(const CHqlBoundExpr & bound)
{
    if (bound.length)
        return bound.length.getLink();

    ITypeInfo * type = bound.expr->queryType();
    if (bound.expr->queryValue())
        return getSizetConstant(type->getStringLen());

    switch (type->getTypeCode())
    {
    case type_varstring:
        {
            HqlExprArray args;
            args.append(*getElementPointer(bound.expr));
            return bindTranslatedFunctionCall(strlenAtom, args);
        }
    case type_varunicode:
        {
            HqlExprArray args;
            args.append(*getElementPointer(bound.expr));
            return bindTranslatedFunctionCall(unicodeStrlenAtom, args);
        }
    case type_set:
    case type_array:
    case type_table:
    case type_groupedtable:
        assertex(!isArrayRowset(type));
        if (bound.count)
            return convertBetweenCountAndSize(bound, false);
        UNIMPLEMENTED;
    case type_utf8:
        {
            assertex(type->getSize() != UNKNOWN_LENGTH);
            HqlExprArray args;
            args.append(*getSizetConstant(type->getSize()));
            args.append(*getElementPointer(bound.expr));
            return bindTranslatedFunctionCall(utf8LengthAtom, args);
        }

    default:
        return getSizetConstant(type->getStringLen());
    }
}

IHqlExpression * HqlCppTranslator::getBoundSize(ITypeInfo * type, IHqlExpression * length, IHqlExpression * data)
{
    type_t tc = type->getTypeCode();

    ITypeInfo * lengthType = length->queryType();
    switch (tc)
    {
    case type_qstring:
        {
            if (length->queryValue())
                return getSizetConstant((size32_t)rtlQStrSize((size32_t)length->queryValue()->getIntValue()));
            HqlExprArray args;
            args.append(*LINK(length));
            return bindTranslatedFunctionCall(qstrSizeAtom, args);
        }
    case type_varstring:
        return adjustValue(length, 1);
    case type_varunicode:
        {
            OwnedHqlExpr temp = adjustValue(length, 1);
            return multiplyValue(temp, 2);
        }
    case type_unicode:
        return multiplyValue(length, 2);
    case type_utf8:
        {
            assertex(data);
            if (data->queryValue())
                return getSizetConstant(data->queryValue()->getSize());

            HqlExprArray args;
            args.append(*LINK(length));
            args.append(*getElementPointer(data));
            return bindTranslatedFunctionCall(utf8SizeAtom, args);
        }
    case type_array:
    case type_set:
        return LINK(length);
    default:
        return LINK(length);
    }
}


IHqlExpression * HqlCppTranslator::getBoundSize(const CHqlBoundExpr & bound)
{
    ITypeInfo * type = bound.expr->queryType();
    if (bound.length)
        return getBoundSize(type, bound.length, bound.expr);

    type_t tc = type->getTypeCode();
    if (tc == type_row)
    {
        if (hasReferenceModifier(type))
            return getSizetConstant(sizeof(void*));

        IHqlExpression * record = ::queryRecord(type);
        ColumnToOffsetMap * map = queryRecordOffsetMap(record);
        if (map->isFixedWidth())
            return getSizetConstant(map->getFixedRecordSize());

        //call meta function mm.queryRecordSize(&row)
        StringBuffer metaInstance, temp;
        buildMetaForRecord(metaInstance, record);
        temp.append(metaInstance).append(".getRecordSize(");
        OwnedHqlExpr rowAddr = getPointer(bound.expr);
        generateExprCpp(temp, rowAddr);
        temp.append(")");
        return createQuoted(temp.str(), LINK(sizetType));
    }

    if (type->getSize() != UNKNOWN_LENGTH)
        return getSizetConstant(type->getSize());
    OwnedHqlExpr length = getBoundLength(bound);
    return getBoundSize(type, length, bound.expr);
}

IHqlExpression * HqlCppTranslator::getFirstCharacter(IHqlExpression * source)
{
    if (source->getOperator() == no_constant)
    {
        StringBuffer temp;
        source->queryValue()->getStringValue(temp);
        return createUIntConstant((unsigned char)temp.charAt(0));
    }

    return createValue(no_index, makeCharType(), LINK(source), getZero());
}


IHqlExpression * HqlCppTranslator::getElementPointer(IHqlExpression * source)
{
    ITypeInfo * srcType = source->queryType();
    switch (srcType->getTypeCode())
    {
        case type_string:
        case type_data:
        case type_qstring:
        case type_varstring:
        case type_unicode:
        case type_utf8:
        case type_varunicode:
        case type_set:
            break;
        default:
            throwUnexpectedType(srcType);
    }

    if (source->getOperator() == no_constant)
        return LINK(source);

    OwnedHqlExpr pointer = getPointer(source);
    return ensureIndexable(pointer);
}

/* All in params: NOT linked */
IHqlExpression * HqlCppTranslator::getIndexedElementPointer(IHqlExpression * source, IHqlExpression * index)
{
    ITypeInfo * srcType = source->queryType();
    switch (srcType->getTypeCode())
    {
        case type_string:
        case type_data:
        case type_qstring:
        case type_varstring:
        case type_unicode:
        case type_utf8:
        case type_varunicode:
            break;
        default:
            throwUnexpectedType(srcType);
    }

    if (!index)
        return getElementPointer(source);

    IValue * value = index->queryValue();
    if (value && value->getIntValue() == 0)
        return getElementPointer(source);

    ITypeInfo * refType = LINK(srcType);
    if (!srcType->isReference())
        refType = makeReferenceModifier(refType);

    OwnedHqlExpr temp;
    if (srcType->getTypeCode() == type_utf8)
    {
        HqlExprArray args;
        args.append(*LINK(index));
        args.append(*getElementPointer(source));
        temp.setown(bindTranslatedFunctionCall(utf8SizeAtom, args));
        index = temp;
    }

    //special case string indexing
    if (source->getOperator() != no_constant)
    {
        if (!srcType->isReference() && !hasWrapperModifier(srcType))
            return createValue(no_address, refType, createValue(no_index, makeCharType(), ensureIndexable(source), LINK(index)));
    }

    return createValue(no_add, refType, ensureIndexable(source), LINK(index));
}


IHqlExpression * HqlCppTranslator::getIndexedElementPointer(IHqlExpression * source, unsigned index)
{
    if (!index)
        return getElementPointer(source);
    OwnedHqlExpr ival =  getSizetConstant(index);
    return getIndexedElementPointer(source, ival);
}


IHqlExpression * HqlCppTranslator::needFunction(_ATOM name)
{
    HqlDummyLookupContext dummyctx(errors);
    return internalScope->lookupSymbol(name, LSFsharedOK, dummyctx);
}


unsigned HqlCppTranslator::processHint(IHqlExpression * expr)
{
    unsigned oldHints = hints;
    _ATOM hint = NULL; // MORE how do I get this?
    if (hint == sizeAtom)
        hints = (hints & ~(HintSpeed|HintSize)) | HintSize;
    else if (hint == speedAtom)
        hints = (hints & ~(HintSize|HintSpeed)) | HintSpeed;
    return oldHints;
}

bool HqlCppTranslator::childrenRequireTemp(BuildCtx & ctx, IHqlExpression * expr, bool includeChildren)
{
    unsigned numArgs = expr->numChildren();
    for (unsigned index = 0; index < numArgs; index++)
        if (requiresTemp(ctx, expr->queryChild(index), includeChildren))
            return true;
    return false;
}

bool HqlCppTranslator::requiresTemp(BuildCtx & ctx, IHqlExpression * expr, bool includeChildren)
{
    switch (expr->getOperator())
    {
    case no_attr:
    case no_attr_link:
    case no_attr_expr:
    case no_quoted:
    case no_variable:
    case no_constant:
    case no_translated:
    case no_matchtext:
    case no_matchunicode:
    case no_matchlength:
    case no_matchattr:
    case no_matchrow:
    case no_matchutf8:
    case no_libraryinput:
        return false;
    case no_getresult:
    case no_getgraphresult:
    case no_workunit_dataset:
        return false;       // if in an activity, then will be in setContext, if not then don't really care
    case no_preservemeta:
        return requiresTemp(ctx, expr->queryChild(0), includeChildren);
    case no_alias:
        {
            if (expr->isPure() && ctx.queryMatchExpr(expr->queryChild(0)))
                return false;
            if (!containsActiveDataset(expr))           // generates a earlier temp even if generating within the onCreate() function
                return false;
            return true;
        }
    case no_select:
        return expr->hasProperty(newAtom);
    case no_field:
        throwUnexpected();
        return false;       // more, depends on whether conditional etc.
    case no_sizeof:
    case no_offsetof:
        return false; /// auto creates one anyway.
    case no_typetransfer:
        switch (expr->queryChild(0)->queryType()->getTypeCode())
        {
        case type_qstring:
        case type_string:
        case type_data:
        case type_varstring:
            break;
        default:
            return true;
        }
        break;
    case no_substring:
        {
            SubStringInfo info(expr);
            if (!info.canGenerateInline() && !expr->hasProperty(quickAtom))
                return true;
            break;
        }
    case no_call:
    case no_externalcall:
        {
            ITypeInfo * type = expr->queryType();
            switch (type->getTypeCode())
            {
            case type_string:
            case type_data:
            case type_qstring:
            case type_varstring:
            case type_unicode:
            case type_varunicode:
            case type_utf8:
                return true;
            }
            break;
        }
    case no_cast:
    case no_implicitcast:
        {
            ITypeInfo * type = expr->queryType();
            IHqlExpression * child = expr->queryChild(0);
            switch (type->getTypeCode())
            {
            case type_string:
            case type_data:
                if (!canRemoveStringCast(type, child->queryType()))
                    return true;
                break;
            case type_varstring:
                if ((type->getSize() != UNKNOWN_LENGTH) || (child->queryType()->getTypeCode() != type_varstring))
                    return true;
                break;
            }
        }
        break;
    case no_eq:
    case no_ne:
    case no_lt:
    case no_le:
    case no_gt:
    case no_ge:
        {
            if (!includeChildren)
                return false;
            unsigned numArgs = expr->numChildren();
            for (unsigned index = 0; index < numArgs; index++)
            {
                OwnedHqlExpr cur = getSimplifyCompareArg(expr->queryChild(index));
                //decimal comparisons can't be short circuited because they might cause a bcd stack overflow.
                if (cur->queryType()->getTypeCode() == type_decimal)
                    return true;
                if (requiresTemp(ctx, cur, true))
                    return true;
            }
            return false;
        }
    case no_mul: 
    case no_div: 
    case no_modulus: 
    case no_add:
    case no_sub:
    case no_and: 
    case no_or:
    case no_xor:
    case no_lshift:
    case no_rshift:
    case no_comma:
    case no_compound:
    case no_band:
    case no_bor:
    case no_bxor:
    case no_pselect:
    case no_index:
    case no_postinc:
    case no_postdec:
    case no_negate: 
    case no_not: 
    case no_bnot: 
    case no_address:
    case no_deref:
    case no_preinc:
    case no_predec:
    case no_if:
    case no_charlen:
        break;
    case no_order:
    case no_crc:
    case no_hash:
    case no_hash32:
    case no_hash64:
    case no_hashmd5:
    case no_abs:
        return true;
    default:
        return true;
    }

    if (includeChildren)
        return childrenRequireTemp(ctx, expr, includeChildren);
    return false;
}


bool HqlCppTranslator::requiresTempAfterFirst(BuildCtx & ctx, IHqlExpression * expr)
{
    unsigned numArgs = expr->numChildren();
    for (unsigned index = 1; index < numArgs; index++)
        if (requiresTemp(ctx, expr->queryChild(index), true))
            return true;
    return false;
}

void HqlCppTranslator::useFunction(IHqlExpression * func)
{
    code->useFunction(func);
}

void HqlCppTranslator::useLibrary(const char * libname)
{
    code->useLibrary(libname);
}

//===========================================================================
static unique_id_t queryInstance = 0;

HqlQueryInstance::HqlQueryInstance()
{
    instance = ++queryInstance;
}

StringBuffer & HqlQueryInstance::queryDllName(StringBuffer & out)
{
    return out.append("query").append(instance);
}
