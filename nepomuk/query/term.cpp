/*
   This file is part of the Nepomuk KDE project.
   Copyright (C) 2007-2010 Sebastian Trueg <trueg@kde.org>

   This library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Library General Public
   License version 2 as published by the Free Software Foundation.

   This library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Library General Public License for more details.

   You should have received a copy of the GNU Library General Public License
   along with this library; see the file COPYING.LIB.  If not, write to
   the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
   Boston, MA 02110-1301, USA.
 */

#include "term.h"
#include "term_p.h"
#include "literalterm.h"
#include "resourceterm.h"
#include "andterm.h"
#include "orterm.h"
#include "negationterm.h"
#include "optionalterm.h"
#include "comparisonterm.h"
#include "resourcetypeterm.h"
#include "literalterm_p.h"
#include "resourceterm_p.h"
#include "andterm_p.h"
#include "orterm_p.h"
#include "negationterm_p.h"
#include "optionalterm_p.h"
#include "comparisonterm_p.h"
#include "resourcetypeterm_p.h"
#include "queryserializer.h"

#include <QtCore/QStringList>
#include <QtCore/QList>
#include <QtCore/QDebug>

#include "property.h"
#include "variant.h"


Nepomuk::Query::Term::Term()
    : d_ptr( new TermPrivate() )
{
}


Nepomuk::Query::Term::Term( const Term& other )
    : d_ptr( other.d_ptr )
{
}


Nepomuk::Query::Term::Term( TermPrivate* d )
    : d_ptr( d )
{
}


Nepomuk::Query::Term::~Term()
{
}


Nepomuk::Query::Term& Nepomuk::Query::Term::operator=( const Term& other )
{
    d_ptr = other.d_ptr;
    return *this;
}


bool Nepomuk::Query::Term::isValid() const
{
    return d_ptr->isValid();
}


Nepomuk::Query::Term::Type Nepomuk::Query::Term::type() const
{
    return d_ptr->m_type;
}


Nepomuk::Query::Term Nepomuk::Query::Term::optimized() const
{
    switch( type() ) {
    case Nepomuk::Query::Term::And:
    case Nepomuk::Query::Term::Or: {
        QList<Nepomuk::Query::Term> subTerms = static_cast<const Nepomuk::Query::GroupTerm&>( *this ).subTerms();
        QList<Nepomuk::Query::Term> newSubTerms;
        QList<Nepomuk::Query::Term>::const_iterator end( subTerms.constEnd() );
        for ( QList<Nepomuk::Query::Term>::const_iterator it = subTerms.constBegin();
              it != end; ++it ) {
            const Nepomuk::Query::Term& t = *it;
            Nepomuk::Query::Term ot = t.optimized();
            QList<Nepomuk::Query::Term> terms;
            if ( ot.type() == type() ) {
                terms = static_cast<const Nepomuk::Query::GroupTerm&>( ot ).subTerms();
            }
            else if( ot.isValid() ) {
                terms += ot;
            }
            Q_FOREACH( const Nepomuk::Query::Term& t, terms ) {
                if( !newSubTerms.contains( t ) )
                    newSubTerms += t;
            }
        }
        if ( newSubTerms.count() == 0 )
            return Nepomuk::Query::Term();
        else if ( newSubTerms.count() == 1 )
            return *newSubTerms.begin();
        else if ( isAndTerm() )
            return Nepomuk::Query::AndTerm( newSubTerms );
        else
            return Nepomuk::Query::OrTerm( newSubTerms );
    }

    case Nepomuk::Query::Term::Negation: {
        Nepomuk::Query::NegationTerm nt = toNegationTerm();
        // a negation in a negation
        if( nt.subTerm().isNegationTerm() )
            return nt.subTerm().toNegationTerm().subTerm().optimized();
        else
            return Nepomuk::Query::NegationTerm::negateTerm( nt.subTerm().optimized() );
    }

    case Nepomuk::Query::Term::Optional: {
        Nepomuk::Query::OptionalTerm ot = toOptionalTerm();
        // remove duplicate optional terms
        if( ot.subTerm().isOptionalTerm() )
            return ot.subTerm().optimized();
        else
            return Nepomuk::Query::OptionalTerm::optionalizeTerm( ot.subTerm().optimized() );
    }

    case Nepomuk::Query::Term::Comparison: {
        Nepomuk::Query::ComparisonTerm ct( toComparisonTerm() );
        ct.setSubTerm( ct.subTerm().optimized() );
        return ct;
    }

    default:
        return *this;
    }
}


bool Nepomuk::Query::Term::isLiteralTerm() const
{
    return type() == Literal;
}


bool Nepomuk::Query::Term::isResourceTerm() const
{
    return type() == Resource;
}


bool Nepomuk::Query::Term::isNegationTerm() const
{
    return type() == Negation;
}


bool Nepomuk::Query::Term::isOptionalTerm() const
{
    return type() == Optional;
}


bool Nepomuk::Query::Term::isAndTerm() const
{
    return type() == And;
}


bool Nepomuk::Query::Term::isOrTerm() const
{
    return type() == Or;
}


bool Nepomuk::Query::Term::isComparisonTerm() const
{
    return type() == Comparison;
}


bool Nepomuk::Query::Term::isResourceTypeTerm() const
{
    return type() == ResourceType;
}


Nepomuk::Query::LiteralTerm Nepomuk::Query::Term::toLiteralTerm() const
{
    if ( isLiteralTerm() ) {
        return *static_cast<const LiteralTerm*>( this );
    }
    else
        return LiteralTerm();
}


Nepomuk::Query::ResourceTerm Nepomuk::Query::Term::toResourceTerm() const
{
    if ( isResourceTerm() )
        return *static_cast<const ResourceTerm*>( this );
    else
        return ResourceTerm();
}


Nepomuk::Query::NegationTerm Nepomuk::Query::Term::toNegationTerm() const
{
    if ( isNegationTerm() )
        return *static_cast<const NegationTerm*>( this );
    else
        return NegationTerm();
}


Nepomuk::Query::OptionalTerm Nepomuk::Query::Term::toOptionalTerm() const
{
    if ( isOptionalTerm() )
        return *static_cast<const OptionalTerm*>( this );
    else
        return OptionalTerm();
}


Nepomuk::Query::AndTerm Nepomuk::Query::Term::toAndTerm() const
{
    if ( isAndTerm() )
        return *static_cast<const AndTerm*>( this );
    else
        return AndTerm();
}


Nepomuk::Query::OrTerm Nepomuk::Query::Term::toOrTerm() const
{
    if ( isOrTerm() )
        return *static_cast<const OrTerm*>( this );
    else
        return OrTerm();
}


Nepomuk::Query::ComparisonTerm Nepomuk::Query::Term::toComparisonTerm() const
{
    if ( isComparisonTerm() )
        return *static_cast<const ComparisonTerm*>( this );
    else
        return ComparisonTerm();
}


Nepomuk::Query::ResourceTypeTerm Nepomuk::Query::Term::toResourceTypeTerm() const
{
    if ( isResourceTypeTerm() )
        return *static_cast<const ResourceTypeTerm*>( this );
    else
        return ResourceTypeTerm();
}


#define CONVERT_AND_RETURN( Class ) \
    if ( !is##Class() )                                       \
        d_ptr = new Class##Private();                         \
    return *static_cast<Class*>( this )

Nepomuk::Query::LiteralTerm& Nepomuk::Query::Term::toLiteralTerm()
{
    CONVERT_AND_RETURN( LiteralTerm );
}


Nepomuk::Query::ResourceTerm& Nepomuk::Query::Term::toResourceTerm()
{
    CONVERT_AND_RETURN( ResourceTerm );
}


Nepomuk::Query::NegationTerm& Nepomuk::Query::Term::toNegationTerm()
{
    CONVERT_AND_RETURN( NegationTerm );
}


Nepomuk::Query::OptionalTerm& Nepomuk::Query::Term::toOptionalTerm()
{
    CONVERT_AND_RETURN( OptionalTerm );
}


Nepomuk::Query::AndTerm& Nepomuk::Query::Term::toAndTerm()
{
    CONVERT_AND_RETURN( AndTerm );
}


Nepomuk::Query::OrTerm& Nepomuk::Query::Term::toOrTerm()
{
    CONVERT_AND_RETURN( OrTerm );
}


Nepomuk::Query::ComparisonTerm& Nepomuk::Query::Term::toComparisonTerm()
{
    CONVERT_AND_RETURN( ComparisonTerm );
}


Nepomuk::Query::ResourceTypeTerm& Nepomuk::Query::Term::toResourceTypeTerm()
{
    CONVERT_AND_RETURN( ResourceTypeTerm );
}


QString Nepomuk::Query::Term::toString() const
{
    return Nepomuk::Query::serializeTerm( *this );
}


// static
Nepomuk::Query::Term Nepomuk::Query::Term::fromString( const QString& s )
{
    return Nepomuk::Query::parseTerm( s );
}


// static
Nepomuk::Query::Term Nepomuk::Query::Term::fromVariant( const Variant& variant )
{
    if( variant.isResource() ) {
        return ResourceTerm( variant.toResource() );
    }
    else if( !variant.isList() ) {
        Soprano::LiteralValue v( variant.variant() );
        if( v.isValid() ) {
            return LiteralTerm( v );
        }
    }

    // fallback: invalid term
    return Term();
}


// static
Nepomuk::Query::Term Nepomuk::Query::Term::fromProperty( const Nepomuk::Types::Property& property, const Nepomuk::Variant& variant )
{
    if( variant.isList() ) {
        AndTerm andTerm;
        Q_FOREACH( const Variant& v, variant.toVariantList() ) {
            andTerm.addSubTerm( fromProperty(property, v) );
        }
        return andTerm;
    }
    else {
        return ComparisonTerm( property, Term::fromVariant(variant), ComparisonTerm::Equal );
    }
}


bool Nepomuk::Query::Term::operator==( const Term& other ) const
{
    return d_ptr->equals( other.d_ptr );
}


bool Nepomuk::Query::Term::operator!=( const Term& other ) const
{
    return !d_ptr->equals( other.d_ptr );
}


QDebug operator<<( QDebug dbg, const Nepomuk::Query::Term& term )
{
    return term.operator<<( dbg );
}


Nepomuk::Query::Term Nepomuk::Query::operator&&( const Term& term1, const Term& term2 )
{
    QList<Term> terms;
    if( term1.isAndTerm() )
        terms << term1.toAndTerm().subTerms();
    else if( term1.isValid() )
        terms << term1;
    if( term2.isAndTerm() )
        terms << term2.toAndTerm().subTerms();
    else if( term2.isValid() )
        terms << term2;

    if( terms.count() == 1 )
        return terms.first();
    else if( terms.count() > 1 )
        return AndTerm( terms );
    else
        return Term();
}


Nepomuk::Query::Term Nepomuk::Query::operator||( const Term& term1, const Term& term2 )
{
    QList<Term> terms;
    if( term1.isOrTerm() )
        terms << term1.toOrTerm().subTerms();
    else if( term1.isValid() )
        terms << term1;
    if( term2.isOrTerm() )
        terms << term2.toOrTerm().subTerms();
    else if( term2.isValid() )
        terms << term2;

    if( terms.count() == 1 )
        return terms.first();
    else if( terms.count() > 1 )
        return OrTerm( terms );
    else
        return Term();
}


Nepomuk::Query::Term Nepomuk::Query::operator!( const Nepomuk::Query::Term& term )
{
    return NegationTerm::negateTerm( term );
}


Nepomuk::Query::ComparisonTerm Nepomuk::Query::operator<( const Nepomuk::Types::Property& property, const Nepomuk::Query::Term& term )
{
    return ComparisonTerm( property, term, ComparisonTerm::Smaller );
}


Nepomuk::Query::ComparisonTerm Nepomuk::Query::operator>( const Nepomuk::Types::Property& property, const Nepomuk::Query::Term& term )
{
    return ComparisonTerm( property, term, ComparisonTerm::Greater );
}


Nepomuk::Query::ComparisonTerm Nepomuk::Query::operator<=( const Nepomuk::Types::Property& property, const Nepomuk::Query::Term& term )
{
    return ComparisonTerm( property, term, ComparisonTerm::SmallerOrEqual );
}


Nepomuk::Query::ComparisonTerm Nepomuk::Query::operator>=( const Nepomuk::Types::Property& property, const Nepomuk::Query::Term& term )
{
    return ComparisonTerm( property, term, ComparisonTerm::GreaterOrEqual );
}


Nepomuk::Query::ComparisonTerm Nepomuk::Query::operator==( const Nepomuk::Types::Property& property, const Nepomuk::Query::Term& term )
{
    return ComparisonTerm( property, term, ComparisonTerm::Equal );
}


Nepomuk::Query::Term Nepomuk::Query::operator!=( const Nepomuk::Types::Property& property, const Nepomuk::Query::Term& term )
{
    return !( property == term );
}


uint Nepomuk::Query::qHash( const Nepomuk::Query::Term& term )
{
    switch( term.type() ) {
    case Nepomuk::Query::Term::Literal:
        return( qHash( term.toLiteralTerm().value().toString() ) );

    case Nepomuk::Query::Term::Comparison:
        return( qHash( term.toComparisonTerm().property().uri().toString() )<<24 |
                qHash( term.toComparisonTerm().subTerm() )<<16 |
                ( uint )term.toComparisonTerm().comparator()<<8 );

    case Nepomuk::Query::Term::Negation:
        return qHash(term.toNegationTerm().subTerm());

    case Nepomuk::Query::Term::Optional:
        return qHash(term.toOptionalTerm().subTerm());

    case Nepomuk::Query::Term::Resource:
        return qHash( term.toResourceTerm().resource().resourceUri() );

    case Nepomuk::Query::Term::ResourceType:
        return qHash( term.toResourceTypeTerm().type().uri() );

    case Nepomuk::Query::Term::And:
    case Nepomuk::Query::Term::Or: {
        uint h = ( uint )term.type();
        QList<Nepomuk::Query::Term> subTerms = static_cast<const GroupTerm&>( term ).subTerms();
        for ( int i = 0; i < subTerms.count(); ++i ) {
            h |= ( qHash( subTerms[i] )<<i );
        }
        return h;
    }

    default:
        return 0;
    }
}


/// We need to overload QSharedDataPointer::clone to make sure the correct subclasses are created
/// when detaching. The default implementation would always call TermPrivate( const TermPrivate& )
template<> Nepomuk::Query::TermPrivate* QSharedDataPointer<Nepomuk::Query::TermPrivate>::clone()
{
    return d->clone();
}


QDebug Nepomuk::Query::Term::operator<<( QDebug dbg ) const
{
    return dbg << toString();
}
