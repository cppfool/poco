//
// StatementExecutor.cpp
//
// $Id: //poco/1.3/Data/PostgreSQL/src/StatementExecutor.cpp#1 $
//
// Library: Data
// Package: PostgreSQL
// Module:  StatementExecutor
//
// Copyright (c) 2008, Applied Informatics Software Engineering GmbH.
// and Contributors.
//
// Permission is hereby granted, free of charge, to any person or organization
// obtaining a copy of the software and accompanying documentation covered by
// this license (the "Software") to use, reproduce, display, distribute,
// execute, and transmit the Software, and to prepare derivative works of the
// Software, and to permit third-parties to whom the Software is furnished to
// do so, all subject to the following:
// 
// The copyright notices in the Software and this entire statement, including
// the above license grant, this restriction and the following disclaimer,
// must be included in all copies of the Software, in whole or in part, and
// all derivative works of the Software, unless such copies or derivative
// works are solely in the form of machine-executable object code generated by
// a source language processor.
// 
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE, TITLE AND NON-INFRINGEMENT. IN NO EVENT
// SHALL THE COPYRIGHT HOLDERS OR ANYONE DISTRIBUTING THE SOFTWARE BE LIABLE
// FOR ANY DAMAGES OR OTHER LIABILITY, WHETHER IN CONTRACT, TORT OR OTHERWISE,
// ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
// DEALINGS IN THE SOFTWARE.
//


#include "Poco/Data/PostgreSQL/StatementExecutor.h"
#include "Poco/Data/PostgreSQL/PostgreSQLTypes.h"

#include "Poco/Format.h"
#include "Poco/UUID.h"
#include "Poco/UUIDGenerator.h"
#include "Poco/NumberParser.h"
#include "Poco/NumberParser.h"
#include "Poco/RegularExpression.h"  // TODO: remove after C++ 11 implementation

//#include <regex> // saved for C++ 11 implementation
#include <algorithm>
#include <set>

namespace
{
	std::size_t countOfPlaceHoldersInSQLStatement(const std::string & aSQLStatement)
	{

	// Find unique placeholders.
	// Unique placeholders allow the same placeholder to be used multiple times in the same statement.

	// NON C++11 implementation

	//if (aSQLStatement.empty())
	//{
	//return 0;
	//}

	// set to hold the unique placeholders ($1, $2, $3, etc.).
	// A set is used because the same placeholder can be used muliple times
	std::set< std::string > placeholderSet;

	Poco::RegularExpression placeholderRE("[$][0-9]+");
	Poco::RegularExpression::Match match = { 0 , 0 }; // Match is a struct, not a class :-(

	std::size_t startingPosition = 0;

	while (match.offset != std::string::npos)
	{
		try
		{
			if (placeholderRE.match(aSQLStatement, startingPosition, match))
			{
				placeholderSet.insert(aSQLStatement.substr(match.offset, match.length));
				startingPosition = match.offset + match.length;
			}
		}
		catch (Poco::RegularExpressionException &)
		{
			break;
		}

	}


/*  C++ 11 implementation

	std::regex const expression("[$][0-9]+");  // match literal dollar signs followed directly by one or more digits

	std::sregex_iterator itr(aSQLStatement.begin(), aSQLStatement.end(), expression);
	std::sregex_iterator eItr;

	// set to hold the unique placeholders ($1, $2, $3, etc.).
	// A set is used because the same placeholder can be used muliple times
	std::set< std::string > placeholderSet;

	while (itr != eItr)
	{
		placeholderSet.insert(itr->str());
		++itr;
	}
*/
	return placeholderSet.size();
	}
} // namespace

namespace Poco {
namespace Data {
namespace PostgreSQL {


StatementExecutor::StatementExecutor(SessionHandle & aSessionHandle)
:	_sessionHandle						(aSessionHandle),
	_state								(STMT_INITED),
	_pResultHandle						(0),
	_countPlaceholdersInSQLStatement	(0),
	_currentRow							(0),
	_affectedRowCount					(0)
{
}


StatementExecutor::~StatementExecutor()
{
	try
	{
		// remove the prepared statement from the session
		if	(	 _sessionHandle.isConnected()
			  && _state >= STMT_COMPILED
			)
		{
			_sessionHandle.deallocatePreparedStatement(_preparedStatementName);
		}

		PQResultClear resultClearer(_pResultHandle);
	}
	catch (...)
	{
	}
}


StatementExecutor::State
StatementExecutor::state() const
{
	return _state;
}

void StatementExecutor::prepare(const std::string & aSQLStatement)
{
	if (! _sessionHandle.isConnected())
	{
		throw NotConnectedException();
	}

	if (_state >= STMT_COMPILED)
	{
		return;
	}

	// clear out the metadata.  One way or another it is now obsolete.
	_countPlaceholdersInSQLStatement = 0;
	_SQLStatement= std::string();
	_preparedStatementName   = std::string();
	_resultColumns.clear();

	// clear out any result data.  One way or another it is now obsolete.
	clearResults();

	// prepare parameters for the call to PQprepare
	const char* ptrCSQLStatement = aSQLStatement.c_str();
	std::size_t countPlaceholdersInSQLStatement = countOfPlaceHoldersInSQLStatement(aSQLStatement);

	Poco::UUIDGenerator & generator = Poco::UUIDGenerator::defaultGenerator();
	Poco::UUID uuid(generator.create()); // time based
	std::string statementName = uuid.toString();
	statementName.insert(0, 1, 'p'); // prepared statement names can't start with a number
	std::replace(statementName.begin(), statementName.end(), '-', 'p');  // PostgreSQL doesn't like dashes in prepared statement names
	const char* pStatementName = statementName.c_str();

	PGresult * ptrPGResult = 0;

	{
		// lock the session
		Poco::FastMutex::ScopedLock mutexLocker(_sessionHandle.mutex());

		// prepare the statement - temporary PGresult returned
		ptrPGResult = PQprepare(_sessionHandle,
								pStatementName,
								ptrCSQLStatement,
								countPlaceholdersInSQLStatement,
								0	// not specifying type Oids
								);

	}

	{
		// setup to clear the result from PQprepare
		PQResultClear resultClearer(ptrPGResult);

		if	(	! ptrPGResult
			 || PQresultStatus(ptrPGResult) != PGRES_COMMAND_OK
			)
		{
			throw StatementException(std::string("postgresql_stmt_prepare error: ") + PQresultErrorMessage (ptrPGResult) + " " + aSQLStatement);
		}

	}

	// Determine what the structure of a statement result will look like

	{
		// lock the session
		Poco::FastMutex::ScopedLock mutexLocker(_sessionHandle.mutex());

		ptrPGResult = PQdescribePrepared(_sessionHandle, pStatementName);
	}

	{
		PQResultClear resultClearer(ptrPGResult);

		if	(! ptrPGResult
			 || PQresultStatus(ptrPGResult) != PGRES_COMMAND_OK
			)
		{
			throw StatementException(std::string("postgresql_stmt_describe error: ") + PQresultErrorMessage (ptrPGResult) + " " + aSQLStatement);
		}

		// remember the structure of the statement result

		int fieldCount = PQnfields(ptrPGResult);
		if (fieldCount < 0)
		{
			fieldCount = 0;
		}

		for (int i = 0; i < fieldCount; ++i)
		{
			int columnLength	= PQfsize(ptrPGResult, i); // TODO: Verify this is correct for all the returned types
			int columnPrecision	= PQfmod(ptrPGResult, i);

			if	(	columnLength < 0		// PostgreSQL confusion correction
				 && columnPrecision > 0
				)
			{
				columnLength	= columnPrecision;
				columnPrecision	= -1;
			}

			_resultColumns.push_back(
				MetaColumn(i,   // position
							PQfname(ptrPGResult, i),   // name
							Poco::Data::MetaColumn::FDT_STRING,  // type - TODO: Map from OIDS to Metacolumn types
							(-1 == columnLength ? 0 : columnLength), // length - NOT CORRECT WHEN RESULTS ARE IN TEXT FORMAT AND DATATYPE IS NOT TEXT
							(-1 == columnPrecision ? 0 : columnPrecision),   // precision
							true // nullable? - no easy way to tell, so assume yes
						)
			);
		}
	}

	_SQLStatement						= aSQLStatement;
	_preparedStatementName				= statementName;
	_countPlaceholdersInSQLStatement	= countPlaceholdersInSQLStatement;

	_state								= STMT_COMPILED;  // must be last

}


void StatementExecutor::bindParams(const InputParameterVector & anInputParameterVector)
{
	if (! _sessionHandle.isConnected())
	{
		throw NotConnectedException();
	}

	if (_state < STMT_COMPILED)
	{
		throw StatementException("Statement is not compiled yet");
	}

	if (anInputParameterVector.size() != _countPlaceholdersInSQLStatement)
	{
		throw StatementException(std::string("incorrect bind parameters count for SQL Statement: ") + _SQLStatement);
	}

	// Just record the input vector for later execution
	_inputParameterVector = anInputParameterVector;
}

void StatementExecutor::execute()
{
	if (! _sessionHandle.isConnected())
	{
		throw NotConnectedException();
	}

	if (_state < STMT_COMPILED)
	{
		throw StatementException("Statement is not compiled yet");
	}

	if	(	_countPlaceholdersInSQLStatement != 0
		 && _inputParameterVector.size() != _countPlaceholdersInSQLStatement
		)
	{
		throw StatementException("Count of Parameters in Statement different than supplied parameters");
	}

	// "transmogrify" the _inputParameterVector to the C format required by PQexecPrepared

	/* - from example
		const char *paramValues[1];
		int paramLengths[1];
		int paramFormats[1];
	*/

	std::vector< const char * > pParameterVector;
	std::vector< int >  parameterLengthVector;
	std::vector< int >  parameterFormatVector;

	InputParameterVector::const_iterator cItr		= _inputParameterVector.begin();
	InputParameterVector::const_iterator cItrEnd	= _inputParameterVector.end();

	for (; cItr != cItrEnd; ++cItr)
	{
		try
		{
			pParameterVector.push_back  (static_cast< const char * >(cItr->pInternalRepresentation()));
			parameterLengthVector.push_back(cItr->size());
			parameterFormatVector.push_back(cItr->isBinary() ? 1 : 0);
		}
		catch (std::bad_alloc&)
		{
			throw StatementException("Memory Allocation Error");
		}
	}

	// clear out any result data.  One way or another it is now obsolete.
	clearResults();

	PGresult * ptrPGResult = 0;

	{
		Poco::FastMutex::ScopedLock mutexLocker(_sessionHandle.mutex());

		/* - from api doc
			PGresult *PQexecPrepared(PGconn *conn,
			const char *stmtName,
			int nParams,
			const char * const *paramValues,
			const int *paramLengths,
			const int *paramFormats,
			int resultFormat);
		*/
		ptrPGResult = PQexecPrepared (_sessionHandle,
									  _preparedStatementName.c_str(),
									  _countPlaceholdersInSQLStatement,
									  _inputParameterVector.size() != 0 ? &pParameterVector[ 0 ]  : 0,
									  _inputParameterVector.size() != 0 ? &parameterLengthVector[ 0 ] : 0,
									  _inputParameterVector.size() != 0 ? &parameterFormatVector[ 0 ] : 0,
									  0 // text based result please!
									);

	}

	// Don't setup to auto clear the result (ptrPGResult).  It is required to retrieve the results later.

	if	(	! ptrPGResult
		 || (PQresultStatus(ptrPGResult) != PGRES_COMMAND_OK
		 && PQresultStatus(ptrPGResult) != PGRES_TUPLES_OK)
		)
	{
		PQResultClear resultClearer(ptrPGResult);

		const char* pSeverity	= PQresultErrorField(ptrPGResult, PG_DIAG_SEVERITY);
		const char* pSQLState	= PQresultErrorField(ptrPGResult, PG_DIAG_SQLSTATE);
		const char* pDetail		= PQresultErrorField(ptrPGResult, PG_DIAG_MESSAGE_DETAIL);
		const char* pHint		= PQresultErrorField(ptrPGResult, PG_DIAG_MESSAGE_HINT);
		const char* pConstraint	= PQresultErrorField(ptrPGResult, PG_DIAG_CONSTRAINT_NAME);

		throw StatementException(std::string("postgresql_stmt_execute error: ")
									+ PQresultErrorMessage (ptrPGResult)
									+ " Severity: "
									+ (pSeverity   ? pSeverity   : "N/A")
									+ " State: "
									+ (pSQLState   ? pSQLState   : "N/A")
									+ " Detail: "
									+ (pDetail ? pDetail : "N/A")
									+ " Hint: "
									+ (pHint   ? pHint   : "N/A")
									+ " Constraint: "
									+ (pConstraint ? pConstraint : "N/A")
								);
	}

	_pResultHandle = ptrPGResult;

	// are there any results?

	int affectedRowCount = 0;

	if (PGRES_TUPLES_OK == PQresultStatus(_pResultHandle))
	{
		affectedRowCount = PQntuples(_pResultHandle);

		if (affectedRowCount >= 0)
		{
			_affectedRowCount = static_cast< std::size_t >(affectedRowCount);
		}
	}
	else
	{	// non Select DML statments also have an affected row count.
		// unfortunately PostgreSQL offers up this count as a char * - go figure!
		const char * pNonSelectAffectedRowCountString = PQcmdTuples(_pResultHandle);
		if (0 != pNonSelectAffectedRowCountString)
		{
			if	(	Poco::NumberParser::tryParse(pNonSelectAffectedRowCountString, affectedRowCount)
				 && affectedRowCount >= 0
				)
			{
				_affectedRowCount = static_cast< std::size_t >(affectedRowCount);
				_currentRow = _affectedRowCount;  // no fetching on these statements!
			}
		}
	}

	_state = STMT_EXECUTED;
}


bool
StatementExecutor::fetch()
{
	if (! _sessionHandle.isConnected())
	{
		throw NotConnectedException();
	}

	if (_state < STMT_EXECUTED)
	{
		throw StatementException("Statement is not yet executed");
	}

	std::size_t countColumns = columnsReturned();

	// first time to fetch?
	if (0 == _outputParameterVector.size())
	{
		// setup a output vector for the results
		_outputParameterVector.resize(countColumns);
	}

	// already retrieved last row?
	if (_currentRow == getAffectedRowCount())
	{
		return false;
	}

	if	(0 == countColumns
		 || PGRES_TUPLES_OK != PQresultStatus(_pResultHandle)
		)
	{
		return false;
	}

	for (std::size_t i = 0; i < countColumns; ++i)
	{
		int fieldLength = PQgetlength(_pResultHandle, static_cast< int > (_currentRow), static_cast< int > (i));

		_outputParameterVector.at(i).setValues(POSTGRESQL_TYPE_STRING,   // TODO - set based on Oid
													PQftype(_pResultHandle, i), // Oid of column
													_currentRow,  // the row number of the result
													PQgetvalue(_pResultHandle, _currentRow, i), // a pointer to the data
													(-1 == fieldLength ? 0 : fieldLength),  // the length of the data returned
													PQgetisnull(_pResultHandle, _currentRow, i) == 1 ? true : false // is the column null
												);
	}

	// advance to next row
	++_currentRow;

	return true;
}


std::size_t
StatementExecutor::getAffectedRowCount() const
{
	return _affectedRowCount;
}

std::size_t
StatementExecutor::columnsReturned() const
{
	return static_cast< std::size_t > (_resultColumns.size());
}

const MetaColumn&
StatementExecutor::metaColumn(std::size_t aPosition) const
{
	if (aPosition >= columnsReturned())
	{
		throw StatementException("Invalid column number for metaColumn");
	}

	return _resultColumns.at(aPosition);
}

const OutputParameter&
StatementExecutor::resultColumn(std::size_t aPosition) const
{
	if (aPosition >= columnsReturned())
	{
		throw StatementException("Invalid column number for resultColumn");
	}

	return _outputParameterVector.at(aPosition);
}


void
StatementExecutor::clearResults()
{
	// clear out any old result first
	{
		PQResultClear resultClearer(_pResultHandle);
	}

	_outputParameterVector.clear();
	_affectedRowCount	= 0;
	_currentRow			= 0;
}


}}}
