/* Internal representation of trace files generated by lcov. */

#ifndef TRACE_H_INCLUDED
#define TRACE_H_INCLUDED

#include <iostream>

#include <string>
#include <map>

class TraceParser;

struct Trace
{
	/* Type used for counters. */
	typedef long counter_t;
	
	Trace();
	
	Trace(const Trace& trace);
	Trace& operator=(const Trace& trace);

	~Trace();

	/* 
	 * Load trace from the stream.
	 * 
	 * If non-empty, 'filename' is used for error reporting.
	 */
	void read(std::istream& is, const char* filename = "");
	/* Same but use already existed parser instead of create new one. */
	void read(std::istream& is, TraceParser& parser, const char* filename = "");
	/* Load trace from file. */
	void read(const char* filename);
	void read(const char* filename, TraceParser& parser);

	/* Store trace to the stream */
	void write(std::ostream& os) const;


	struct FuncInfo;
	struct BranchID;
	struct FileInfo;
	

	/* 
	 * Identificator of files group.
	 * 
	 * It may be used for identify correspondence between coverage
	 * information when compare traces or perform some merging in it.
	 * 
	 * There are at least two ways of grouping coverage information which
	 * are used by lcov/gcov (perhaps, with different options):
	 * 
	 * 1) Each file coverage information is prepended by test name.
	 * 2) Test name is prepend coverage information for source file and
	 * all headers, used in it.
	 * 
	 * In first case, each file is described only once, coverage
	 * information for headers included in several sources is combined.
	 * In the second case, each source file is described only once,
	 * but there are several descriptions for headers file, one for each
	 * source file it included by.
	 * 
	 * Files group identificator process both cases:
	 * in the first case it consist from test name and file name,
	 * in the second case it consist from test name and source file name.
	 */
	struct FileGroupID
	{
		std::string testName;
		/* 
		 * When group contains from one file, this corresponds to name
		 * of that file.
		 * When group contains from several files, this corresponds to
		 * name of source file(".c" extension). Precisely one file
		 * should have this extension.
		 */
		std::string filename;
		
		/* This operator allows to use group identificator as key in map. */
		bool operator<(const FileGroupID& groupID) const;
		/* Pretty printer for error-reporting. */
		friend std::ostream& operator<<(std::ostream& os, const FileGroupID& groupId);
	};
	/* 
	 * Information about group of files.
	 */
	struct FileGroupInfo
	{
		std::map<std::string, FileInfo> files;
	};
	
	/* 
	 * Trace consist of files groups.
	 * 
	 * Groups are delimited with "TN:" directive in the trace file.
	 * 
	 * When read trace from file, 'filename' field of group isn't known
	 * at the start of group. Using pointer in map value allows to
	 * store information about group into 'FileGroupInfo' and then,
	 * when filename become known, insert this information into map
	 * instead of copiing this information.
	 * 
	 * When destroy trace, all information for each group in the trace
	 * is freed automatically.
	 * But when you delete key->value pair from map by hands, it is
	 * needed to delete information explicitly.
	 */
	std::map<FileGroupID, FileGroupInfo*> fileGroups;
	
	/* 
	 * Make every group to contain only one file.
	 * 
	 * All statistic for files with same names and different groups is
	 * joined.
	 */
	void groupFiles(void);

	/* 
	 * Useful functions for calculate statistic for all files.
	 * 
	 * NOTE: Theese functions assume files with same names different,
	 * if they belong to file groups with difference name. So, statistic
	 * for such files sums instead of joined.
	 * 
	 * If you want to assume files with same name identical, use
	 * groupFiles() method before call these functions.
	 */
	int linesTotal(void) const;
	int linesTotalHit(void) const;

	int branchesTotal(void) const;
	int branchesTotalHit(void) const;

	int functionsTotal(void) const;
	int functionsTotalHit(void) const;
	
	class Modifier;
private:
	class TraceBuilder;
};

/* Information about function in file */
struct Trace::FuncInfo
{
	/* 
	 * Line where function starts.
	 * 
	 * Sometimes, gcov misses definition of function, while define
	 * counter for it.
	 * It may occure, e.g., when inline function calls another
	 * inline function: in that case line for caller is not defined
	 * in trace file, but counter is.
	 * Value -1 is used for signal that situation.
	 */
	int lineStart;
	/* Hit counter for function */
	counter_t counter;
	
	/* 'counter should be set after constructor '*/
	FuncInfo(int lineStart): lineStart(lineStart) {}
};

/* Branch identificator in the file */
struct Trace::BranchID
{
	/* Line of the branch in the file */
	int line;
	/* 
	 * Block and branch numbers - gcov internals for uniquely
	 * identify branch.
	 */
	int blockNumber;
	int branchNumber;
	
	BranchID(int line, int blockNumber, int branchNumber):
		line(line), blockNumber(blockNumber), branchNumber(branchNumber) {}

	/* This operator allows to use branch identificator as key in map. */
	bool operator<(const BranchID& branchID) const;
	/* Simple pretty-printing for error reporting */
	friend std::ostream& operator<<(std::ostream& os, const Trace::BranchID& branchID);
};

/* Information about one file(source or header) in trace */
struct Trace::FileInfo
{
	/* Function information for each function name in file. */
	std::map<std::string, FuncInfo> functions;
	/* Counter for each line in file. */
	std::map<int, counter_t> lines;
	/* 
	 * Counter for each branch in file.
	 * 
	 * value -1 corresponds to '-' in BRDA directive in trace file.
	 */
	std::map<BranchID, counter_t> branches;
	
	/* Useful functions for calculate per-file statistic */
	int linesTotal(void) const;
	int linesTotalHit(void) const;

	int branchesTotal(void) const;
	int branchesTotalHit(void) const;

	int functionsTotal(void) const;
	int functionsTotalHit(void) const;

	class Modifier;
};


#endif /* TRACE_H_INCLUDED */
