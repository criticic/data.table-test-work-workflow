#include "data.table.h"

static void finalizer(SEXP p)
{
  SEXP x;
  R_len_t n, l, tl;
  if(!R_ExternalPtrAddr(p)) internal_error(__func__, "didn't receive an ExternalPtr"); // # nocov
  p = R_ExternalPtrTag(p);
  if (!isString(p)) internal_error(__func__, "ExternalPtr doesn't see names in tag"); // # nocov
  l = LENGTH(p);
  tl = TRUELENGTH(p);
  if (l<0 || tl<l) internal_error(__func__, "l=%d, tl=%d", l, tl); // # nocov
  n = tl-l;
  if (n==0) {
    // gc's ReleaseLargeFreeVectors() will have reduced R_LargeVallocSize by the correct amount
    // already, so nothing to do (but almost never the case).
    return;
  }
  x = PROTECT(allocVector(INTSXP, 50));  // 50 so it's big enough to be on LargeVector heap. See NodeClassSize in memory.c:allocVector
                                         // INTSXP rather than VECSXP so that GC doesn't inspect contents after LENGTH (thanks to Karl Miller, Jul 2015)
  SETLENGTH(x,50+n*2*sizeof(void *)/4);  // 1*n for the names, 1*n for the VECSXP itself (both are over allocated).
  UNPROTECT(1);
  return;
}

void setselfref(SEXP x) {
  if(!INHERITS(x, char_datatable))  return; // #5286
  SEXP p;
  // Store pointer to itself so we can detect if the object has been copied. See
  // ?copy for why copies are not just inefficient but cause a problem for over-allocated data.tables.
  // Called from C only, not R level, so returns void.
  setAttrib(x, SelfRefSymbol, p=R_MakeExternalPtr(
    R_NilValue,                  // for identical() to return TRUE. identical() doesn't look at tag and prot
    PROTECT(getAttrib(x, R_NamesSymbol)),  // to detect if names has been replaced and its tl lost, e.g. setattr(DT,"names",...)
    PROTECT(R_MakeExternalPtr(   // to avoid an infinite loop in object.size(), if prot=x here
      x,                         // to know if this data.table has been copied by attr<-, names<-, etc.
      R_NilValue,                // this tag and prot currently unused
      R_NilValue
    ))
  ));
  R_RegisterCFinalizerEx(p, finalizer, FALSE);
  UNPROTECT(2);

/*
  *  base::identical doesn't check prot and tag of EXTPTR, just that the ptr itself is the
     same in both objects. R_NilValue is always equal to R_NilValue.  R_NilValue is a memory
     location constant within an R session, but can vary from session to session. So, it
     looks like a pointer to a user looking at attributes(DT), but they might wonder how it
     works if they realise the selfref of all data.tables all point to the same address (rather
     than to the table itself which would be reasonable to expect given the attribute's name).
  *  p=NULL rather than R_NilValue works too, other than we need something other than NULL
     so we can detect tables loaded from disk (which set p to NULL, see 5.13 of R-exts).
  *  x is wrapped in another EXTPTR because of object.size (called by tables(), and by users).
     If the prot (or tag) was x directly it sends object.size into an infinite loop and then
     "segfault from C stack overflow" (object.size does count tag and prot, unlike identical,
     but doesn't count what's pointed to).
  *  Could use weak reference possibly, but the fact that they can get be set to R_NilValue
     by gc (?) didn't seem appropriate.
  *  If the .internal.selfref attribute is removed (e.g. by user code), nothing will break, but
     an extra copy will just be taken on next :=, with warning, with a new selfref.
  *  object.size will count size of names twice, but that's ok as only small.
  *  Thanks to Steve L for suggesting ExtPtr for this, rather than the previous REALSXP
     vector which required data.table to do a show/hide dance in a masked identical.
*/
}

/* There are two reasons the finalizer doesn't restore the LENGTH to TRUELENGTH. i) The finalizer
happens too late after GC has already released the memory, and ii) copies by base R (e.g.
[<- in write.table just before test 894) allocate at length LENGTH but copy the TRUELENGTH over.
If the finalizer sets LENGTH to TRUELENGTH, that's a fail as it wasn't really allocated at
TRUELENGTH when R did the copy.
Karl Miller suggested an ENVSXP so that restoring LENGTH in finalizer should work. This is the
closest I got to getting it to pass all tests :

  SEXP env = PROTECT(allocSExp(ENVSXP));
  defineVar(SelfRefSymbol, x, env);
  defineVar(R_NamesSymbol, getAttrib(x, R_NamesSymbol), env);
  setAttrib(x, SelfRefSymbol, p = R_MakeExternalPtr(
    R_NilValue,         // for identical() to return TRUE. identical() doesn't look at tag and prot
    R_NilValue, //getAttrib(x, R_NamesSymbol), // to detect if names has been replaced and its tl lost, e.g. setattr(DT,"names",...)
    PROTECT(            // needed when --enable-strict-barrier it seems, iiuc. TO DO: test under that flag and remove if not needed.
      env               // wrap x in env to avoid an infinite loop in object.size() if prot=x were here
    )
  ));
  R_RegisterCFinalizerEx(p, finalizer, FALSE);
  UNPROTECT(2);

Then in finalizer:
  SETLENGTH(names, tl)
  SETLENGTH(dt, tl)

and that finalizer indeed now happens before the GC releases memory (thanks to the env wrapper).

However, we still have problem (ii) above and it didn't pass tests involving base R copies.

We really need R itself to start setting TRUELENGTH to be the allocated length and then
for GC to release TRUELENGTH not LENGTH.  Would really tidy this up.

Moved out of ?setkey Details section in 1.12.2 (Mar 2019). Revisit this w.r.t. to recent versions of R.
  The problem (for \code{data.table}) with the copy by \code{key<-} (other than
  being slower) is that \R doesn't maintain the over-allocated truelength, but it
  looks as though it has. Adding a column by reference using \code{:=} after a
  \code{key<-} was therefore a memory overwrite and eventually a segfault; the
  over-allocated memory wasn't really there after \code{key<-}'s copy. \code{data.table}s now have an attribute \code{.internal.selfref} to catch and warn about such copies.
  This attribute has been implemented in a way that is friendly with
  \code{identical()} and \code{object.size()}.
*/

static int _selfrefok(SEXP x, Rboolean checkNames, Rboolean verbose) {
  SEXP v, p, tag, prot, names;
  v = getAttrib(x, SelfRefSymbol);
  if (v==R_NilValue || TYPEOF(v)!=EXTPTRSXP) {
    // .internal.selfref missing is expected and normal for i) a pre v1.7.8 data.table loaded
    //  from disk, and ii) every time a new data.table is over-allocated for the first time.
    //  Not being an extptr is for when users construct a data.table via structure() using dput, post
    //  a question, and find the extptr doesn't parse so put quotes around it (for example).
    //  In both cases the selfref is not ok.
    return 0;
  }
  p = R_ExternalPtrAddr(v);
  if (p==NULL) {
    if (verbose) Rprintf(_("The data.table internal attributes of this table are invalid. This is expected and normal for a data.table loaded from disk. Please remember to always setDT() immediately after loading to prevent unexpected behavior. If this table was not loaded from disk or you've already run setDT(), please report to the data.table issue tracker.\n"));
    return -1;
  }
  if (!isNull(p)) internal_error(__func__, ".internal.selfref ptr is neither NULL nor R_NilValue"); // # nocov
  tag = R_ExternalPtrTag(v);
  if (!(isNull(tag) || isString(tag))) internal_error(__func__, ".internal.selfref tag is neither NULL nor a character vector"); // # nocov
  names = getAttrib(x, R_NamesSymbol);
  if (names!=tag && isString(names) && !ALTREP(names))  // !ALTREP for #4734
    SET_TRUELENGTH(names, LENGTH(names));
    // R copied this vector not data.table; it's not actually over-allocated. It looks over-allocated
    // because R copies the original vector's tl over despite allocating length.
  prot = R_ExternalPtrProtected(v);
  if (TYPEOF(prot) != EXTPTRSXP)   // Very rare. Was error(_(".internal.selfref prot is not itself an extptr")).
    return 0;                      // # nocov ; see http://stackoverflow.com/questions/15342227/getting-a-random-internal-selfref-error-in-data-table-for-r
  if (x!=R_ExternalPtrAddr(prot) && !ALTREP(x))
    SET_TRUELENGTH(x, LENGTH(x));  // R copied this vector not data.table, it's not actually over-allocated
  return checkNames ? names==tag : x==R_ExternalPtrAddr(prot);
}

static Rboolean selfrefok(SEXP x, Rboolean verbose) {   // for readability
  return(_selfrefok(x, FALSE, verbose)==1);
}
static Rboolean selfrefnamesok(SEXP x, Rboolean verbose) {
  return(_selfrefok(x, TRUE, verbose)==1);
}

static SEXP shallow(SEXP dt, SEXP cols, R_len_t n)
{
  // NEW: cols argument to specify the columns to shallow copy on. If NULL, all columns.
  // called from alloccol where n is checked carefully, or from shallow() at R level
  // where n is set to truelength (i.e. a shallow copy only with no size change)
  int protecti=0;
  SEXP newdt = PROTECT(allocVector(VECSXP, n)); protecti++;   // to do, use growVector here?
  SHALLOW_DUPLICATE_ATTRIB(newdt, dt);

  // TO DO: keepattr() would be faster, but can't because shallow isn't merely a shallow copy. It
  //        also increases truelength. Perhaps make that distinction, then, and split out, but marked
  //        so that the next change knows to duplicate.
  //        keepattr() also merely points to the entire attributes list and thus doesn't allow replacing
  //        some of its elements.

  // We copy all attributes that refer to column names so that calling setnames on either
  // the original or the shallow copy doesn't break anything.
  SEXP index = PROTECT(getAttrib(dt, sym_index)); protecti++;
  setAttrib(newdt, sym_index, shallow_duplicate(index));

  SEXP sorted = PROTECT(getAttrib(dt, sym_sorted)); protecti++;
  setAttrib(newdt, sym_sorted, duplicate(sorted));

  SEXP names = PROTECT(getAttrib(dt, R_NamesSymbol)); protecti++;
  SEXP newnames = PROTECT(allocVector(STRSXP, n)); protecti++;
  const int l = isNull(cols) ? LENGTH(dt) : length(cols);
  if (isNull(cols)) {
    for (int i=0; i<l; ++i) SET_VECTOR_ELT(newdt, i, VECTOR_ELT(dt,i));
    if (length(names)) {
      if (length(names) < l) internal_error(__func__, "length(names)>0 but <length(dt)"); // # nocov
      for (int i=0; i<l; ++i) SET_STRING_ELT(newnames, i, STRING_ELT(names,i));
    }
    // else an unnamed data.table is valid e.g. unname(DT) done by ggplot2, and .SD may have its names cleared in dogroups, but shallow will always create names for data.table(NULL) which has 100 slots all empty so you can add to an empty data.table by reference ok.
  } else {
    for (int i=0; i<l; ++i) SET_VECTOR_ELT(newdt, i, VECTOR_ELT(dt,INTEGER(cols)[i]-1));
    if (length(names)) {
      // no need to check length(names) < l here. R-level checks if all value
      // in 'cols' are valid - in the range of 1:length(names(x))
      for (int i=0; i<l; ++i) SET_STRING_ELT( newnames, i, STRING_ELT(names,INTEGER(cols)[i]-1) );
    }
  }
  setAttrib(newdt, R_NamesSymbol, newnames);
  // setAttrib appears to change length and truelength, so need to do that first _then_ SET next,
  // otherwise (if the SET were were first) the 100 tl is assigned to length.
  SETLENGTH(newnames,l);
  SET_TRUELENGTH(newnames,n);
  SETLENGTH(newdt,l);
  SET_TRUELENGTH(newdt,n);
  setselfref(newdt);
  UNPROTECT(protecti);
  return(newdt);
}

// Wrapped in a function so the same message is issued for the data.frame case at the R level
void warn_matrix_column(/* 1-indexed */ int i) {
  warning(_("Some columns are a multi-column type (such as a matrix column), for example column %d. setDT will retain these columns as-is but subsequent operations like grouping and joining may fail. Please consider as.data.table() instead which will create a new column for each embedded column."), i);
}

// input validation for setDT() list input; assume is.list(x) was tested in R
SEXP setdt_nrows(SEXP x)
{
  int base_length = 0;
  bool test_matrix_cols = true;

  for (R_len_t i = 0; i < LENGTH(x); ++i) {
    SEXP xi = VECTOR_ELT(x, i);
    /* allow NULL columns to be created by setDT(list) even though they are not really allowed
     *   many operations still work in the presence of NULL columns and it might be convenient
     *   e.g. in package eplusr which calls setDT on a list when parsing JSON. Operations which
     *   fail for NULL columns will give helpful error at that point, #3480 and #3471 */
    if (isNull(xi)) continue;
    if (Rf_inherits(xi, "POSIXlt")) {
      error(_("Column %d has class 'POSIXlt'. Please convert it to POSIXct (using as.POSIXct) and run setDT() again. We do not recommend the use of POSIXlt at all because it uses 40 bytes to store one date."), i+1);
    }
    SEXP dim_xi = PROTECT(getAttrib(xi, R_DimSymbol));
    R_len_t len_xi, n_dim = length(dim_xi);
    if (n_dim) {
      if (test_matrix_cols && n_dim > 1) {
        warn_matrix_column(i+1);
        test_matrix_cols = false;
      }
      len_xi = INTEGER(dim_xi)[0];
    } else {
      len_xi = length(xi);
    }
    UNPROTECT(1);
    if (!base_length) {
      base_length = len_xi;
    } else if (len_xi != base_length) {
      error(_("All elements in argument 'x' to 'setDT' must be of equal length, but input %d has length %d whereas the first non-empty input had length %d"), i+1, len_xi, base_length);
    }
  }
  return ScalarInteger(base_length);
}

SEXP alloccol(SEXP dt, R_len_t n, Rboolean verbose)
{
  SEXP names, klass;   // klass not class at request of pydatatable because class is reserved word in C++, PR #3129
  R_len_t l, tl;
  if (isNull(dt)) error(_("alloccol has been passed a NULL dt"));
  if (TYPEOF(dt) != VECSXP) error(_("dt passed to alloccol isn't type VECSXP"));
  klass = getAttrib(dt, R_ClassSymbol);
  if (isNull(klass)) error(_("dt passed to alloccol has no class attribute. Please report result of traceback() to data.table issue tracker."));
  l = LENGTH(dt);
  names = getAttrib(dt,R_NamesSymbol);
  // names may be NULL when null.data.table() passes list() to alloccol for example.
  // So, careful to use length() on names, not LENGTH().
  if (length(names)!=l) internal_error(__func__, "length of names (%d) is not length of dt (%d)", length(names),l); // # nocov
  if (!selfrefok(dt,verbose))
    return shallow(dt,R_NilValue,(n>l) ? n : l);  // e.g. test 848 and 851 in R > 3.0.2
    // added (n>l) ? ... for #970, see test 1481.
  // TO DO:  test realloc names if selfrefnamesok (users can setattr(x,"name") themselves for example.
  // if (TRUELENGTH(getAttrib(dt,R_NamesSymbol))!=tl)
  //    internal_error(__func__, "tl of dt passes checks, but tl of names (%d) != tl of dt (%d)", tl, TRUELENGTH(getAttrib(dt,R_NamesSymbol))); // # nocov

  tl = TRUELENGTH(dt);
  // R <= 2.13.2 and we didn't catch uninitialized tl somehow
  if (tl<0) internal_error(__func__, "tl of class is marked but tl<0"); // # nocov
  if (tl>0 && tl<l) internal_error(__func__, "tl (%d) < l (%d) but tl of class is marked", tl, l); // # nocov
  if (tl>l+10000) warning(_("tl (%d) is greater than 10,000 items over-allocated (l = %d). If you didn't set the datatable.alloccol option to be very large, please report to data.table issue tracker including the result of sessionInfo()."),tl,l);
  if (n>tl) return(shallow(dt,R_NilValue,n)); // usual case (increasing alloc)
  if (n<tl && verbose) Rprintf(_("Attempt to reduce allocation from %d to %d ignored. Can only increase allocation via shallow copy. Please do not use DT[...]<- or DT$someCol<-. Use := inside DT[...] instead."),tl,n);
        // otherwise the finalizer can't clear up the Large Vector heap
  return(dt);
}

int checkOverAlloc(SEXP x)
{
  if (isNull(x))
    error(_("Has getOption('datatable.alloccol') somehow become unset? It should be a number, by default 1024."));
  if (!isInteger(x) && !isReal(x))
    error(_("getOption('datatable.alloccol') should be a number, by default 1024. But its type is '%s'."), type2char(TYPEOF(x)));
  if (LENGTH(x) != 1)
    error(_("getOption('datatable.alloccol') is a numeric vector ok but its length is %d. Its length should be 1."), LENGTH(x));
  int ans = asInteger(x);
  if (ans<0)
    error(_("getOption('datatable.alloccol')==%d.  It must be >=0 and not NA."), ans);
  return ans;
}

SEXP alloccolwrapper(SEXP dt, SEXP overAllocArg, SEXP verbose) {
  if (!IS_TRUE_OR_FALSE(verbose))
    error(_("%s must be TRUE or FALSE"), "verbose");
  int overAlloc = checkOverAlloc(overAllocArg);
  SEXP ans = PROTECT(alloccol(dt, length(dt)+overAlloc, LOGICAL(verbose)[0]));

  for(R_len_t i = 0; i < LENGTH(ans); i++) {
    // clear names; also excluded by copyMostAttrib(). Primarily for data.table and as.data.table, but added here centrally (see #103).
    setAttrib(VECTOR_ELT(ans, i), R_NamesSymbol, R_NilValue);

    // But don't clear dim and dimnames. Because as from 1.12.4 we keep the matrix column as-is and ask user to use as.data.table to
    // unpack matrix columns when they really need to; test 2089.2
    // setAttrib(VECTOR_ELT(ans, i), R_DimSymbol, R_NilValue);
    // setAttrib(VECTOR_ELT(ans, i), R_DimNamesSymbol, R_NilValue);
  }

  UNPROTECT(1);
  return ans;
}

SEXP shallowwrapper(SEXP dt, SEXP cols) {
  // selfref will be FALSE on manually created data.table, e.g., via dput() or structure()
  if (!selfrefok(dt, FALSE)) {
    int n = isNull(cols) ? length(dt) : length(cols);
    return(shallow(dt, cols, n));
  } else return(shallow(dt, cols, TRUELENGTH(dt)));
}

SEXP truelength(SEXP x) {
  return ScalarInteger(isNull(x) ? 0 : TRUELENGTH(x));
}

SEXP selfrefokwrapper(SEXP x, SEXP verbose) {
  return ScalarInteger(_selfrefok(x,FALSE,LOGICAL(verbose)[0]));
}

int *_Last_updated = NULL;

SEXP assign(SEXP dt, SEXP rows, SEXP cols, SEXP newcolnames, SEXP values)
{
  // For internal use only by := in [.data.table, and set()
  // newcolnames : add these columns (if any)
  // cols : column names or numbers corresponding to the values to set
  // rows : row numbers to assign
  R_len_t numToDo, targetlen, vlen, oldncol, oldtncol, coln, protecti=0, newcolnum, indexLength;
  SEXP targetcol, nullint, s, colnam, tmp, key, index, a, assignedNames, indexNames;
  bool verbose=GetVerbose();
  int ndelete=0;  // how many columns are being deleted
  const char *c1, *tc1, *tc2;
  int *buf, indexNo;
  if (isNull(dt)) error(_("assign has been passed a NULL dt"));
  if (TYPEOF(dt) != VECSXP) error(_("dt passed to assign isn't type VECSXP"));
  if (islocked(dt))
    error(_(".SD is locked. Updating .SD by reference using := or set are reserved for future use. Use := in j directly. Or use copy(.SD) as a (slow) last resort, until shallow() is exported."));

  // We allow set() on data.frame too; e.g. package Causata uses set() on a data.frame in tests/testTransformationReplay.R
  // := is only allowed on a data.table. However, the ":=" = stop(...) message in data.table.R will have already
  // detected use on a data.frame before getting to this point.
  // For data.frame, can use set() on existing columns but not add new ones because DF are not over-allocated.
  bool isDataTable = INHERITS(dt, char_datatable);
  if (!isDataTable && !INHERITS(dt, char_dataframe))
    internal_error(__func__, "dt is not a data.table or data.frame");  // # nocov

  oldncol = LENGTH(dt);
  SEXP names = PROTECT(getAttrib(dt, R_NamesSymbol)); protecti++;
  if (isNull(names)) error(_("dt passed to assign has no names"));
  if (length(names)!=oldncol)
    internal_error(__func__, "length of names (%d) is not length of dt (%d)", length(names), oldncol); // # nocov
  const int nrow = LENGTH(dt) ? length(VECTOR_ELT(dt,0)) :
                                (isNewList(values) && length(values) && (length(values)==length(cols)) ? length(VECTOR_ELT(values,0)) : length(values));
  //                            ^ when null data.table the new nrow becomes the fist column added
  //                                                                    ^ distinguish between adding list column or multiple columns at once, #5738
  if (isNull(rows)) {
    numToDo = nrow;
    targetlen = nrow;
    if (verbose) Rprintf(_("Assigning to all %d rows\n"), nrow);
    // fast way to assign to whole column, without creating 1:nrow(x) vector up in R, or here in C
  } else {
    if (isReal(rows)) {
      rows = PROTECT(coerceVector(rows, INTSXP)); protecti++;
      if (verbose)
        Rprintf(_("Coerced %s from numeric to integer. Passing integer directly may be more efficient, e.g., 2L rather than 2"), "i");
    }
    if (!isInteger(rows))
      error(_("i is type '%s'. Must be integer, or numeric is coerced with warning. If i is a logical subset, simply wrap with which(), and take the which() outside the loop if possible for efficiency."), type2char(TYPEOF(rows)));
    targetlen = length(rows);
    numToDo = 0;
    const int *rowsd = INTEGER(rows);
    for (int i=0; i<targetlen; ++i) {
      if ((rowsd[i]<0 && rowsd[i]!=NA_INTEGER) || rowsd[i]>nrow)
        error(_("i[%d] is %d which is out of range [1,nrow=%d]"), i+1, rowsd[i], nrow);  // set() reaches here (test 2005.2); := reaches the same error in subset.c first
      if (rowsd[i]>=0) numToDo++;
    }
    if (verbose) Rprintf(_("Assigning to %d row subset of %d rows\n"), numToDo, nrow);
    // TODO: include in message if any rows are assigned several times (e.g. by=.EACHI with dups in i)
    if (numToDo==0) {
      // isString(cols) is exclusive to calls from set()
      if (!length(newcolnames) && !isString(cols)) {
        *_Last_updated = 0;
        UNPROTECT(protecti);
        return(dt); // all items of rows either 0 or NA. !length(newcolnames) for #759
      }
      if (verbose) Rprintf(Pl_(length(newcolnames),
                               "Added %d new column initialized with all-NA\n",
                               "Added %d new columns initialized with all-NA\n"),
                           length(newcolnames));
    }
  }
  if (!length(cols)) {
    if (verbose) Rprintf(_("length(LHS)==0; no columns to delete or assign RHS to."));   // test 1295 covers
    *_Last_updated = 0;
    UNPROTECT(protecti);
    return(dt);
  }
  // FR #2077 - set able to add new cols by reference
  if (isString(cols)) {
    PROTECT(tmp = chmatch(cols, names, 0)); protecti++;
    buf = (int *) R_alloc(length(cols), sizeof(*buf));
    int k=0;
    for (int i=0; i<length(cols); ++i) {
      if (INTEGER(tmp)[i] == 0) buf[k++] = i;
    }
    if (k>0) {
      if (!isDataTable) error(_("set() on a data.frame is for changing existing columns, not adding new ones. Please use a data.table for that. data.table's are over-allocated and don't shallow copy."));
      newcolnames = PROTECT(allocVector(STRSXP, k)); protecti++;
      for (int i=0; i<k; ++i) {
        SET_STRING_ELT(newcolnames, i, STRING_ELT(cols, buf[i]));
        INTEGER(tmp)[buf[i]] = oldncol+i+1;
      }
    }
    cols = tmp;
  } else {
    if (isReal(cols)) {
      cols = PROTECT(coerceVector(cols, INTSXP)); protecti++;
      if (verbose)
        Rprintf(_("Coerced %s from numeric to integer. Passing integer directly may be more efficient, e.g., 2L rather than 2"), "j");
    }
    if (!isInteger(cols))
      error(_("j is type '%s'. Must be integer, character, or numeric is coerced with warning."), type2char(TYPEOF(cols)));
  }
  if (any_duplicated(cols,FALSE)) error(_("Can't assign to the same column twice in the same query (duplicates detected)."));
  if (!isNull(newcolnames) && !isString(newcolnames)) error(_("newcolnames is supplied but isn't a character vector"));
  if (Rf_inherits(values, "POSIXlt")) {
    warning(_("Values of type POSIXlt detected and converted to POSIXct. We do not recommend the use of POSIXlt at all because it typically takes more than 6 times the storage as an equivalent POSIXct column. Use as.POSIXct() to avoid this warning."));
    values = PROTECT(eval(PROTECT(lang2(sym_as_posixct, values)), R_GlobalEnv)); protecti+=2;
  }
  bool RHS_list_of_columns = TYPEOF(values)==VECSXP && length(cols)>1;  // initial value; may be revised below
  if (verbose)
    Rprintf("RHS_list_of_columns == %s\n", RHS_list_of_columns ? "true" : "false"); // # notranslate
  if (TYPEOF(values)==VECSXP && length(cols)==1 && length(values)==1) {
    SEXP item = VECTOR_ELT(values,0);
    if (isNull(item) || length(item)==1 || length(item)==targetlen) {
      RHS_list_of_columns=true;
      if (verbose) Rprintf(_("RHS_list_of_columns revised to true because RHS list has 1 item which is NULL, or whose length %d is either 1 or targetlen (%d). Please unwrap RHS.\n"), length(item), targetlen);
    }
  }
  if (RHS_list_of_columns) {
    if (length(values)==0)
      error(_("Supplied %d columns to be assigned an empty list (which may be an empty data.table or data.frame since they are lists too). To delete multiple columns use NULL instead. To add multiple empty list columns, use list(list())."), length(cols));
    if (length(values)!=length(cols)) {
      if (length(values)==1) {   // test 351.1; c("colA","colB"):=list(13:15) uses 13:15 for both columns
        values = VECTOR_ELT(values,0);
        RHS_list_of_columns = false;
        if (verbose) Rprintf(_("Recycling single RHS list item across %d columns. Please unwrap RHS.\n"), length(cols));
      } else {
        error(_("Supplied %d columns to be assigned %d items. Please see NEWS for v1.12.2."), length(cols), length(values));
      }
    }
  }
  // Check all inputs :
  for (int i=0; i<length(cols); ++i) {
    coln = INTEGER(cols)[i];
    if (coln<1 || coln>oldncol+length(newcolnames)) {
      if (!isDataTable) error(_("Item %d of column numbers in j is %d which is outside range [1,ncol=%d]. set() on a data.frame is for changing existing columns, not adding new ones. Please use a data.table for that."), i+1, coln, oldncol);
      else error(_("Item %d of column numbers in j is %d which is outside range [1,ncol=%d]. Use column names instead in j to add new columns."), i+1, coln, oldncol);
    }
    coln--;
    SEXP thisvalue = RHS_list_of_columns ? VECTOR_ELT(values, i) : values;
    if (isNull(thisvalue) && !isNull(rows)) {
      if (coln >= oldncol)
        error(_("Doubly-invalid attempt to delete a non-existent column while also providing i"));
      if (TYPEOF(VECTOR_ELT(dt, coln)) == VECSXP)
        error(_("Invalid attempt to delete a list column while also providing i; did you intend to add NULL to those rows instead? If so, use list_col := list(list(NULL)).")); // #5526
      error(_("When deleting columns, i should not be provided"));  // #1082, #3089
    }
    if (coln+1 <= oldncol) colnam = STRING_ELT(names,coln);
    else colnam = STRING_ELT(newcolnames,coln-length(names));
    if (coln+1 <= oldncol && isNull(thisvalue)) continue;  // delete existing column(s) afterwards, near end of this function
    vlen = length(thisvalue);
    //if (vlen<1 && nrow>0) {
    if (coln+1 <= oldncol && nrow>0 && vlen<1 && numToDo>0) { // numToDo > 0 fixes #2829, see test 1911
      error(_("RHS of assignment to existing column '%s' is zero length but not NULL. If you intend to delete the column use NULL. Otherwise, the RHS must have length > 0; e.g., NA_integer_. If you are trying to change the column type to be an empty list column then, as with all column type changes, provide a full length RHS vector such as vector('list',nrow(DT)); i.e., 'plonk' in the new column."), CHAR(STRING_ELT(names,coln)));
    }
    if (coln+1 > oldncol && TYPEOF(thisvalue)!=VECSXP) {  // list() is ok for new columns
      newcolnum = coln-length(names);
      if (newcolnum<0 || newcolnum>=length(newcolnames))
        internal_error(__func__, "length(newcolnames)=%d, length(names)=%d, coln=%d", length(newcolnames), length(names), coln); // # nocov
      if (isNull(thisvalue)) {
        warning(_("Tried to assign NULL to column '%s', but this column does not exist to remove"), CHAR(STRING_ELT(newcolnames,newcolnum)));
        continue;
      }
      // RHS of assignment to new column is zero length but we'll use its type to create all-NA column of that type
    }
    {
      int j;
      if (isMatrix(thisvalue) && (j=INTEGER(getAttrib(thisvalue, R_DimSymbol))[1]) > 1)  // matrix passes above (considered atomic vector)
        warning(_("%d column matrix RHS of := will be treated as one vector"), j);
    }
    const SEXP existing = (coln+1)<=oldncol ? VECTOR_ELT(dt,coln) : R_NilValue;
    if (isFactor(existing) &&
      !isString(thisvalue) && TYPEOF(thisvalue)!=INTSXP && TYPEOF(thisvalue)!=LGLSXP && !isReal(thisvalue) && !isNewList(thisvalue)) {  // !=INTSXP includes factor
      error(_("Can't assign to column '%s' (type 'factor') a value of type '%s' (not character, factor, integer or numeric)"),
            CHAR(STRING_ELT(names,coln)),type2char(TYPEOF(thisvalue)));
    }
    if (nrow>0 && targetlen>0 && vlen>1 && vlen!=targetlen && !(TYPEOF(existing)==VECSXP && targetlen==1)) {
      // We allow assigning objects of arbitrary to single items of list columns for convenience.
      // Note that isNewList(R_NilValue) is true so it needs to be !(TYPEOF(existing)==VECSXP) above
      error(_("Supplied %d items to be assigned to %d items of column '%s'. If you wish to 'recycle' the RHS please use rep() to make this intent clear to readers of your code."), vlen, targetlen, CHAR(colnam));
    }
  }
  // having now checked the inputs, from this point there should be no errors so we can now proceed to
  // modify DT by reference. Other than if new columns are being added and the allocVec() fails with
  // out-of-memory. In that case the user will receive hard halt and know to rerun.
  if (length(newcolnames)) {
    oldtncol = TRUELENGTH(dt);   // TO DO: oldtncol can be just called tl now, as we won't realloc here any more.

    if (oldtncol<oldncol) {
      if (oldtncol==0) error(_("This data.table has either been loaded from disk (e.g. using readRDS()/load()) or constructed manually (e.g. using structure()). Please run setDT() or setalloccol() on it first (to pre-allocate space for new columns) before assigning by reference to it."));   // #2996
      internal_error(__func__, "oldtncol(%d) < oldncol(%d)", oldtncol, oldncol); // # nocov
    }
    if (oldtncol>oldncol+10000L) warning(_("truelength (%d) is greater than 10,000 items over-allocated (length = %d). See ?truelength. If you didn't set the datatable.alloccol option very large, please report to data.table issue tracker including the result of sessionInfo()."),oldtncol, oldncol);
    if (oldtncol < oldncol+LENGTH(newcolnames))
      internal_error(__func__, "input dt has not been allocated enough column slots. l=%d, tl=%d, adding %d", oldncol, oldtncol, LENGTH(newcolnames));  // # nocov
    if (!selfrefnamesok(dt,verbose))
      error(_("It appears that at some earlier point, names of this data.table have been reassigned. Please ensure to use setnames() rather than names<- or colnames<-. Otherwise, please report to data.table issue tracker."));  // # nocov
      // Can growVector at this point easily enough, but it shouldn't happen in first place so leave it as
      // strong error message for now.
    else if (TRUELENGTH(names) != oldtncol)
      // Use (long long) to cast R_xlen_t to a fixed type to robustly avoid -Wformat compiler warnings, see #5768, PRId64 didn't work
      internal_error(__func__, "selfrefnames is ok but tl names [%lld] != tl [%d]", (long long)TRUELENGTH(names), oldtncol);  // # nocov
    if (!selfrefok(dt, verbose)) // #6410 setDT(dt) and subsequent attr<- can lead to invalid selfref
      error(_("It appears that at some earlier point, attributes of this data.table have been reassigned. Please use setattr(DT, name, value) rather than attr(DT, name) <- value. If that doesn't apply to you, please report your case to the data.table issue tracker."));
    SETLENGTH(dt, oldncol+LENGTH(newcolnames));
    SETLENGTH(names, oldncol+LENGTH(newcolnames));
    for (int i=0; i<LENGTH(newcolnames); ++i)
      SET_STRING_ELT(names,oldncol+i,STRING_ELT(newcolnames,i));
    // truelengths of both already set by alloccol
    if (oldncol==0) {
      // adding columns to null data.table needs row.names set, #4597
      SEXP rn;
      PROTECT(rn = allocVector(INTSXP, 2)); protecti++;
      INTEGER(rn)[0] = NA_INTEGER;
      INTEGER(rn)[1] = -nrow;
      setAttrib(dt, R_RowNamesSymbol, rn);
    }
  }
  for (int i=0; i<length(cols); ++i) {
    coln = INTEGER(cols)[i]-1;
    SEXP thisvalue = RHS_list_of_columns ? VECTOR_ELT(values, i) : values;
    if (TYPEOF(thisvalue)==NILSXP) {
      if (!isNull(rows)) internal_error(__func__, "earlier error 'When deleting columns, i should not be provided' did not happen"); // # nocov
      ndelete++;
      continue;   // delete column(s) afterwards, below this loop
    }
    vlen = length(thisvalue);
    if (length(rows)==0 && targetlen==vlen && (vlen>0 || nrow==0)) {
      if (  MAYBE_SHARED(thisvalue) ||  // set() protects the NAMED of atomic vectors from .Call setting arguments to 2 by wrapping with list
         (TYPEOF(values)==VECSXP && i>LENGTH(values)-1) || // recycled RHS would have columns pointing to others, #185.
         (TYPEOF(values)!=VECSXP && i>0) || // assigning the same values to a second column. Have to ensure a copy #2540
         ALTREP(thisvalue) // Some ALTREP wrappers have survived to here, e.g. #5400
         ) {
        if (verbose) {
          Rprintf(_("RHS for item %d has been duplicated because MAYBE_REFERENCED==%d MAYBE_SHARED==%d ALTREP==%d, but then is being plonked. length(values)==%d; length(cols)==%d\n"),
                  i+1, MAYBE_REFERENCED(thisvalue), MAYBE_SHARED(thisvalue), ALTREP(thisvalue), length(values), length(cols));
        }
        thisvalue = copyAsPlain(thisvalue);   // PROTECT not needed as assigned as element to protected list below.
      } else {
        if (verbose) Rprintf(_("Direct plonk of unnamed RHS, no copy. MAYBE_REFERENCED==%d, MAYBE_SHARED==%d\n"), MAYBE_REFERENCED(thisvalue), MAYBE_SHARED(thisvalue));  // e.g. DT[,a:=as.character(a)] as tested by 754.5
      }
      SET_VECTOR_ELT(dt, coln, thisvalue);                 // plonk new column in as it's already the correct length
      setAttrib(thisvalue, R_NamesSymbol, R_NilValue);     // clear names such as  DT[,a:=mapvector[a]]
      setAttrib(thisvalue, R_DimSymbol, R_NilValue);       // so that matrix is treated as vector
      setAttrib(thisvalue, R_DimNamesSymbol, R_NilValue);  // the 3rd of the 3 attribs not copied by copyMostAttrib, for consistency.
      continue;
    }

    if (coln+1 > oldncol) {  // new column
      SET_VECTOR_ELT(dt, coln, targetcol=allocNAVectorLike(thisvalue, nrow));
      // initialize with NAs for when 'rows' is a subset and it doesn't touch
      // do not try to save the time to NA fill (contiguous branch free assign anyway) since being
      // sure all items will be written to (isNull(rows), length(rows), vlen<1, targetlen) is not worth the risk.
      if (isVectorAtomic(thisvalue)) copyMostAttrib(thisvalue,targetcol);  // class etc but not names
      // else for lists (such as data.frame and data.table) treat them as raw lists and drop attribs
      if (vlen<1) continue;   // e.g. DT[,newcol:=integer()] (adding new empty column)
    } else {                 // existing column
      targetcol = VECTOR_ELT(dt,coln);
    }
    const char *ret = memrecycle(targetcol, rows, 0, targetlen, thisvalue, 0, -1, coln+1, CHAR(STRING_ELT(names, coln)));
    if (ret) warning("%s", ret); // # notranslate
  }

  *_Last_updated = numToDo;  // the updates have taken place with no error, so update .Last.updated now
  assignedNames = PROTECT(allocVector(STRSXP, LENGTH(cols))); protecti++;
  for (int i=0; i<LENGTH(cols); ++i) SET_STRING_ELT(assignedNames,i,STRING_ELT(names,INTEGER(cols)[i]-1));
  key = getAttrib(dt, sym_sorted);
  if (length(key)) {
    // if assigning to at least one key column, the key is truncated to one position before the first changed column.
    //any() and subsetVector() don't seem to be exposed by R API at C level, so this is done here long hand.
    PROTECT(tmp = chin(key, assignedNames)); protecti++;
    int newKeyLength = length(key);
    for (int i=0; i<LENGTH(tmp); ++i) if (LOGICAL(tmp)[i]) {
      // If a key column is being assigned to, set newKeyLength to the key element before since everything after that may have changed in order.
      newKeyLength = i;
      break;
    }
    if(newKeyLength == 0){
      // no valid key columns remain, remove the key
      setAttrib(dt, sym_sorted, R_NilValue);
    } else if (newKeyLength < length(key)){
      // new key is shorter than original one. Reassign
      PROTECT(tmp = allocVector(STRSXP, newKeyLength)); protecti++;
      for (int i=0; i<newKeyLength; ++i) SET_STRING_ELT(tmp, i, STRING_ELT(key, i));
      setAttrib(dt, sym_sorted, tmp);
    }
    //else: no key column changed, nothing to be done
  }
  index = getAttrib(dt, install("index"));
  if (index != R_NilValue) {
    s = ATTRIB(index);
    indexNo = 0;
    // get a vector with all index names
    PROTECT(indexNames = allocVector(STRSXP, xlength(s))); protecti++;
    while(s != R_NilValue){
      SET_STRING_ELT(indexNames, indexNo, PRINTNAME(TAG(s)));
      indexNo++;
      s = CDR(s);
    }
    s = ATTRIB(index); // reset to first element
    indexNo = 0;
    while(s != R_NilValue) {
      a = TAG(s);
      indexLength = xlength(CAR(s));
      tc1 = c1 = CHAR(PRINTNAME(a));  // the index name; e.g. "__col1__col2"
      if (*tc1!='_' || *(tc1+1)!='_') {
        // fix for #1396
        if (verbose) {
          Rprintf(_("Dropping index '%s' as it doesn't have '__' at the beginning of its name. It was very likely created by v1.9.4 of data.table.\n"), tc1);
        }
        setAttrib(index, a, R_NilValue);
        indexNo++;
        s = CDR(s);
        continue; // with next index
      }
      tc1 += 2; // tc1 always marks the start of a key column
      if (!*tc1) internal_error(__func__, "index name ends with trailing __"); // # nocov
      // check the position of the first appearance of an assigned column in the index.
      // the new index will be truncated to this position.
      char *s4 = malloc(strlen(c1) + 3);
      if (!s4) {
        internal_error(__func__, "Couldn't allocate memory for s4"); // # nocov
      }
      memcpy(s4, c1, strlen(c1));
      memset(s4 + strlen(c1), '\0', 1);
      strcat(s4, "__"); // add trailing '__' to newKey so we can search for pattern '__colName__' also at the end of the index.
      int newKeyLength = strlen(c1);
      for(int i = 0; i < xlength(assignedNames); i++){
        tc2 = CHAR(STRING_ELT(assignedNames, i));
        char *s5 = malloc(strlen(tc2) + 5); //4 * '_' + \0
        if (!s5) {
          free(s4);                                                  // # nocov
          internal_error(__func__, "Couldn't allocate memory for s5"); // # nocov
        }
        memset(s5, '_', 2);
        memset(s5 + 2, '\0', 1);
        strcat(s5, tc2);
        strcat(s5, "__");
        tc2 = strstr(s4, s5);
        if(tc2 == NULL){ // column is not part of key
          free(s5);
          continue;
        }
        if(tc2 - s4 < newKeyLength){ // new column match is before last match
          newKeyLength = tc2 - s4;
        }
        free(s5);
      }
      memset(s4 + newKeyLength, '\0', 1); // truncate the new key to the new length
      if(newKeyLength == 0){ // no valid key column remains. Drop the key
        setAttrib(index, a, R_NilValue);
        SET_STRING_ELT(indexNames, indexNo, NA_STRING);
        if (verbose) {
          Rprintf(_("Dropping index '%s' due to an update on a key column\n"), c1+2);
        }
      } else if(newKeyLength < strlen(c1)) {
        SEXP s4Str = PROTECT(mkString(s4));
        if(indexLength == 0 && // shortened index can be kept since it is just information on the order (see #2372)
           LOGICAL(chin(s4Str, indexNames))[0] == 0) {// index with shortened name not present yet
          SET_TAG(s, install(s4));
          SET_STRING_ELT(indexNames, indexNo, mkChar(s4));
          if (verbose)
            Rprintf(_("Shortening index '%s' to '%s' due to an update on a key column\n"), c1+2, s4 + 2);
        } else { // indexLength > 0 || shortened name present already
          // indexLength > 0 indicates reordering. Drop it to avoid spurious reordering in non-indexed columns (#2372)
          // shortened name already present indicates that index needs to be dropped to avoid duplicate indices.
          setAttrib(index, a, R_NilValue);
          SET_STRING_ELT(indexNames, indexNo, NA_STRING);
          if (verbose)
            Rprintf(_("Dropping index '%s' due to an update on a key column\n"), c1+2);
        }
        UNPROTECT(1); // s4Str
      } //else: index is not affected by assign: nothing to be done
      free(s4);
      indexNo ++;
      s = CDR(s);
    }
  }
  if (ndelete) {
    // delete any columns assigned NULL (there was a 'continue' earlier in loop above)
    int *tt = (int *)R_alloc(ndelete, sizeof(*tt));
    const int *colsd=INTEGER(cols), ncols=length(cols), ndt=length(dt);
    for (int i=0, k=0; i<ncols; ++i) {   // find which ones to delete and put them in tt
      // Aside: a new column being assigned NULL (something odd to do) would have been warned above, added above, and now deleted. Just
      //        easier to code it this way; e.g. so that other columns may be added or removed ok by the same query.
      coln = colsd[i]-1;
      SEXP thisvalue = RHS_list_of_columns ? VECTOR_ELT(values, i) : values;
      if (isNull(thisvalue)) tt[k++] = coln;
    }
    R_isort(tt, ndelete);  // sort the column-numbers-to-delete into ascending order
    for (int i=0; i<ndelete-1; ++i) {
      if (tt[i]>=tt[i+1])
        internal_error(__func__, "%d column numbers to delete not now in strictly increasing order. No-dups were checked earlier.", i); // # nocov
    }
    for (int i=tt[0], j=1, k=tt[0]+1;  i<ndt-ndelete;  ++i, ++k) {  // i moves up from the first non-deleted column and is the target of write
      while (j<ndelete && k==tt[j]) { j++; k++; }                   // move k up to the next non-deleted column; j is the next position in tt
      SET_VECTOR_ELT(dt,    i, VECTOR_ELT(dt,    k));
      SET_STRING_ELT(names, i, STRING_ELT(names, k));
    }
    for (int i=ndt-ndelete; i<ndt; ++i) {   // blank out the ndelete slots at the end
      SET_VECTOR_ELT(dt, i, R_NilValue);
      SET_STRING_ELT(names, i, NA_STRING);  // release reference to the CHARSXP
    }
    SETLENGTH(dt,    ndt-ndelete);
    SETLENGTH(names, ndt-ndelete);
    if (LENGTH(names)==0) {
      // That was last column deleted, leaving NULL data.table, so we need to reset .row_names, so that it really is the NULL data.table.
      PROTECT(nullint=allocVector(INTSXP, 0)); protecti++;
      setAttrib(dt, R_RowNamesSymbol, nullint);  // i.e. .set_row_names(0)
    }
  }
  UNPROTECT(protecti);
  return(dt);  // needed for `*tmp*` mechanism (when := isn't used), and to return the new object after a := for compound syntax.
}

static char memrecycle_message[1000]; // returned to rbindlist so it can prefix with which one of the list of data.table-like objects

const char *columnDesc(int colnum, const char *colname) {
  static char column_desc[sizeof(memrecycle_message)]; // can contain column names, hence relatively large allocation.
  snprintf(column_desc, sizeof(memrecycle_message), _("(column %d named '%s')"), colnum, colname);
  return column_desc;
}

const char *memrecycle(const SEXP target, const SEXP where, const int start, const int len, SEXP source, const int sourceStart, const int sourceLen, const int colnum, const char *colname)
// like memcpy but recycles single-item source
// 'where' a 1-based INTEGER vector subset of target to assign to, or NULL or integer()
// assigns to target[start:start+len-1] or target[where[start:start+len-1]] where start is 0-based
// if sourceLen==-1 then all of source is used (if it is 1 item then it is recycled, or its length must match) for convenience to avoid
//   having to use length(source) (repeating source expression) in each call
// sourceLen==1 is used in dogroups to recycle the group values into ans to match the nrow of each group's result; sourceStart is set to each group value row.
{
  if (len<1) return NULL;
  int slen = sourceLen>=0 ? sourceLen : length(source); // since source may get reassigned to a scalar, we should not mark it as const
  if (slen==0) return NULL;
  if (sourceStart<0 || sourceStart+slen>length(source))
    internal_error(__func__, "sourceStart=%d sourceLen=%d length(source)=%d", sourceStart, sourceLen, length(source)); // # nocov
  if (!length(where) && start+len>length(target))
    internal_error(__func__, "start=%d len=%d length(target)=%d", start, len, length(target)); // # nocov
  const int soff = sourceStart;
  if (slen>1 && slen!=len && (!isNewList(target) || isNewList(source)))
    internal_error(__func__, "recycle length error not caught earlier. slen=%d len=%d", slen, len); // # nocov
  // Internal error because the column has already been added to the DT, so length mismatch should have been caught before adding the column.
  // for 5647 this used to limit slen to len, but no longer
  if (colname==NULL)
    internal_error(__func__, "received NULL colname"); // # nocov
  *memrecycle_message = '\0';
  int protecti=0;
  const bool sourceIsFactor=isFactor(source), targetIsFactor=isFactor(target);
  const bool sourceIsI64=isReal(source) && INHERITS(source, char_integer64);
  const bool targetIsI64=isReal(target) && INHERITS(target, char_integer64);
  if (sourceIsFactor || targetIsFactor) {
    if (!targetIsFactor) {
      if (!isString(target) && !isNewList(target))
        error(_("Cannot assign 'factor' to '%s'. Factors can only be assigned to factor, character or list columns."), type2char(TYPEOF(target)));
      // else assigning factor to character is left to later below, avoiding wasteful asCharacterFactor
    } else if (!sourceIsFactor && !isString(source)) {
      // target is factor
      if (allNA(source, false)) {  // return false for list and other types that allNA does not support
        source = ScalarLogical(NA_LOGICAL); slen = 1; // a global constant in R and won't allocate; fall through to regular zero-copy coerce
      } else if (isInteger(source) || isReal(source)) {
        // allow assigning level numbers to factor columns; test 425, 426, 429 and 1945
        const int nlevel = length(getAttrib(target, R_LevelsSymbol));
        if (isInteger(source)) {
          const int *sd = INTEGER(source);
          for (int i=0; i<slen; ++i) {
            const int val = sd[i+soff];
            if ((val<1 && val!=NA_INTEGER) || val>nlevel) {
              if (colnum == 0)
                error(_("Assigning factor numbers to target vector. But %d is outside the level range [1,%d]"), val, nlevel);
              else
                error(_("Assigning factor numbers to column %d named '%s'. But %d is outside the level range [1,%d]"), colnum, colname, val, nlevel);
            }
          }
        } else {
          const double *sd = REAL(source);
          for (int i=0; i<slen; ++i) {
            const double val = sd[i+soff];
            // Since nlevel is an int, val < 1 || val > nlevel will deflect UB guarded against in PR #5832
            if (!ISNAN(val) && (val < 1 || val > nlevel || val != (int)val)) {
              if (colnum == 0)
                error(_("Assigning factor numbers to target vector. But %f is outside the level range [1,%d], or is not a whole number."), val, nlevel);
              else
                error(_("Assigning factor numbers to column %d named '%s'. But %f is outside the level range [1,%d], or is not a whole number."), colnum, colname, val, nlevel);
            }
          }
        }
        // Now just let the valid level numbers fall through to regular assign by BODY below
      } else {
        error(_("Cannot assign '%s' to 'factor'. Factor columns can be assigned factor, character, NA in any type, or level numbers."), type2char(TYPEOF(source)));
      }
    } else {
      // either factor or character being assigned to factor column
      SEXP targetLevels = PROTECT(getAttrib(target, R_LevelsSymbol)); protecti++;
      SEXP sourceLevels = source;  // character source
      if (sourceIsFactor) { sourceLevels=PROTECT(getAttrib(source, R_LevelsSymbol)); protecti++; }
      sourceLevels = PROTECT(coerceUtf8IfNeeded(sourceLevels)); protecti++;
      if (!sourceIsFactor || !R_compute_identical(sourceLevels, targetLevels, 0)) {  // !sourceIsFactor for test 2115.6
        const int nTargetLevels=length(targetLevels), nSourceLevels=length(sourceLevels);
        targetLevels = PROTECT(coerceUtf8IfNeeded(targetLevels)); protecti++;
        const SEXP *targetLevelsD=STRING_PTR_RO(targetLevels), *sourceLevelsD=STRING_PTR_RO(sourceLevels);
        SEXP newSource = PROTECT(allocVector(INTSXP, length(source))); protecti++;
        savetl_init();
        for (int k=0; k<nTargetLevels; ++k) {
          const SEXP s = targetLevelsD[k];
          const int tl = TRUELENGTH(s);
          if (tl>0) {
            savetl(s);
          } else if (tl<0) {
            // # nocov start
            for (int j=0; j<k; ++j) SET_TRUELENGTH(s, 0);  // wipe our negative usage and restore 0
            savetl_end();                                  // then restore R's own usage (if any)
            internal_error(__func__, "levels of target are either not unique or have truelength<0"); // # nocov
            // # nocov end
          }
          SET_TRUELENGTH(s, -k-1);
        }
        int nAdd = 0;
        for (int k=0; k<nSourceLevels; ++k) {
          const SEXP s = sourceLevelsD[k];
          const int tl = TRUELENGTH(s);
          if (tl>=0) {
            if (!sourceIsFactor && s==NA_STRING) continue; // don't create NA factor level when assigning character to factor; test 2117
            if (tl>0) savetl(s);
            SET_TRUELENGTH(s, -nTargetLevels-(++nAdd));
          } // else, when sourceIsString, it's normal for there to be duplicates here
        }
        const int nSource = length(source);
        int *newSourceD = INTEGER(newSource);
        if (sourceIsFactor) {
          const int *sourceD = INTEGER(source);
          for (int i=0; i<nSource; ++i) {  // convert source integers to refer to target levels
            const int val = sourceD[i];
            newSourceD[i] = val==NA_INTEGER ? NA_INTEGER : -TRUELENGTH(sourceLevelsD[val-1]); // retains NA factor levels here via TL(NA_STRING); e.g. ordered factor
          }
        } else {
          const SEXP *sourceD = STRING_PTR_RO(source);
          for (int i=0; i<nSource; ++i) {  // convert source integers to refer to target levels
            const SEXP val = sourceD[i];
            newSourceD[i] = val==NA_STRING ? NA_INTEGER : -TRUELENGTH(val);
          }
        }
        source = newSource;
        for (int k=0; k<nTargetLevels; ++k) SET_TRUELENGTH(targetLevelsD[k], 0);  // don't need those anymore
        if (nAdd) {
          // cannot grow the levels yet as that would be R call which could fail to alloc and we have no hook to clear up
          SEXP *temp = malloc(sizeof(*temp) * nAdd);
          if (!temp) {
            // # nocov start
            for (int k=0; k<nSourceLevels; ++k) SET_TRUELENGTH(sourceLevelsD[k], 0);
            savetl_end();
            error(_("Unable to allocate working memory of %zu bytes to combine factor levels"), nAdd*sizeof(SEXP *));
            // # nocov end
          }
          for (int k=0, thisAdd=0; thisAdd<nAdd; ++k) {   // thisAdd<nAdd to stop early when the added ones are all reached
            SEXP s = sourceLevelsD[k];
            int tl = TRUELENGTH(s);
            if (tl) {  // tl negative here
              if (tl != -nTargetLevels-thisAdd-1) internal_error(__func__, "extra level check sum failed"); // # nocov
              temp[thisAdd++] = s;
              SET_TRUELENGTH(s,0);
            }
          }
          savetl_end();
          setAttrib(target, R_LevelsSymbol, targetLevels=growVector(targetLevels, nTargetLevels + nAdd));
          for (int k=0; k<nAdd; ++k) {
            SET_STRING_ELT(targetLevels, nTargetLevels+k, temp[k]);
          }
          free(temp);
        } else {
          // all source levels were already in target levels, but not with the same integers; we're done
          savetl_end();
        }
        // now continue, but with the mapped integers in the (new) source
      }
    }
  } else if (isString(source) && !isString(target) && !isNewList(target)) {
    if (colnum == 0)
      warning(_("Coercing 'character' RHS to '%s' to match the type of target vector."), targetIsI64?"integer64":type2char(TYPEOF(target)));
    else
      warning(_("Coercing 'character' RHS to '%s' to match the type of column %d named '%s'."), targetIsI64?"integer64":type2char(TYPEOF(target)), colnum, colname);
    // this "Coercing ..." warning first to give context in case coerceVector warns 'NAs introduced by coercion'
    // and also because 'character' to integer/double coercion is often a user mistake (e.g. wrong target column, or wrong
    // variable on RHS) which they are more likely to appreciate than find inconvenient
    source = PROTECT(coerceVector(source, TYPEOF(target))); protecti++;
  } else if (isNewList(source) && !isNewList(target)) {
    if (targetIsI64) {
      if (colnum == 0)
        error(_("Cannot coerce 'list' RHS to 'integer64' to match the type of target vector."));
      else
        error(_("Cannot coerce 'list' RHS to 'integer64' to match the type of column %d named '%s'."), colnum, colname);
      // because R's coerceVector doesn't know about integer64
    }
    // as in base R; e.g. let as.double(list(1,2,3)) work but not as.double(list(1,c(2,4),3))
    // relied on by NNS, simstudy and table.express; tests 1294.*
    if (colnum == 0)
      warning(_("Coercing 'list' RHS to '%s' to match the type of target vector."), type2char(TYPEOF(target)));
    else
      warning(_("Coercing 'list' RHS to '%s' to match the type of column %d named '%s'."), type2char(TYPEOF(target)), colnum, colname);
    source = PROTECT(coerceVector(source, TYPEOF(target))); protecti++;
  } else if ((TYPEOF(target)!=TYPEOF(source) || targetIsI64!=sourceIsI64) && !isNewList(target)) {
    if (GetVerbose()>=3) {
      // only take the (small) cost of GetVerbose() (search of options() list) when types don't match
      const char *sourceType = sourceIsI64 ? "integer64" : type2char(TYPEOF(source));
      const char *targetType = targetIsI64 ? "integer64" : type2char(TYPEOF(target));
      if (colnum == 0)
        Rprintf(_("Zero-copy coerce when assigning '%s' to '%s' target vector.\n"), sourceType, targetType);
      else
        Rprintf(_("Zero-copy coerce when assigning '%s' to column %d named '%s' which is '%s'.\n"), sourceType, colnum, colname, targetType);
    }
    // The following checks are up front here, otherwise we'd need them twice in the two branches
    //   inside BODY that cater for 'where' or not. Maybe there's a way to merge the two macros in future.
    // The idea is to do these range checks without calling coerceVector() (which allocates)

    #define CHECK_RANGE(STYPE, RFUN, COND, FMTVAL, FMT) {{                                                                  \
      const STYPE *sd = (const STYPE *)RFUN(source);                                                                        \
      for (int i=0; i<slen; ++i) {                                                                                          \
        const STYPE val = sd[i+soff];                                                                                       \
        if (COND) {                                                                                                         \
          const char *sType = sourceIsI64 ? "integer64" : type2char(TYPEOF(source));                                        \
          const char *tType = targetIsI64 ? "integer64" : type2char(TYPEOF(target));                                        \
          snprintf(memrecycle_message, sizeof(memrecycle_message), FMT,                                                     \
            FMTVAL, sType, i+1, tType,                                                                                      \
            /* NB: important for () to be part of the translated string as a signal of nominative case to translators */    \
            colnum == 0 ? _("(target vector)") : columnDesc(colnum, colname));                                              \
          /* string returned so that rbindlist/dogroups can prefix it with which item of its list this refers to  */        \
          break;                                                                                                            \
        }                                                                                                                   \
      }                                                                                                                     \
    } break; }

    switch(TYPEOF(target)) {
    case LGLSXP:
      switch (TYPEOF(source)) {
      case RAWSXP:  CHECK_RANGE(Rbyte, RAW,    val!=0 && val!=1,                                        val, _("%d (type '%s') at RHS position %d taken as TRUE when assigning to type '%s' %s"))
      case INTSXP:  CHECK_RANGE(int, INTEGER,  val!=0 && val!=1 && val!=NA_INTEGER,                     val, _("%d (type '%s') at RHS position %d taken as TRUE when assigning to type '%s' %s"))
      case REALSXP: if (sourceIsI64)
                    CHECK_RANGE(int64_t, REAL, val!=0 && val!=1 && val!=NA_INTEGER64,                   val, _("%"PRId64" (type '%s') at RHS position %d taken as TRUE when assigning to type '%s' %s"))
              else  CHECK_RANGE(double, REAL,  !ISNAN(val) && val!=0.0 && val!=1.0,                     val, _("%f (type '%s') at RHS position %d taken as TRUE when assigning to type '%s' %s"))
      } break;
    case RAWSXP:
      switch (TYPEOF(source)) {
      case INTSXP:  CHECK_RANGE(int, INTEGER,  val<0 || val>255,                                        val, _("%d (type '%s') at RHS position %d taken as 0 when assigning to type '%s' %s"))
      case REALSXP: if (sourceIsI64)
                    CHECK_RANGE(int64_t, REAL, val<0 || val>255,                                        val, _("%"PRId64" (type '%s') at RHS position %d taken as 0 when assigning to type '%s' %s"))
              else  CHECK_RANGE(double, REAL,  !R_FINITE(val) || val<0.0 || val>256.0 || (int)val!=val, val, _("%f (type '%s') at RHS position %d either truncated (precision lost) or taken as 0 when assigning to type '%s' %s"))
      } break;
    case INTSXP:
      switch (TYPEOF(source)) {
      case REALSXP: if (sourceIsI64)
                    CHECK_RANGE(int64_t, REAL, val!=NA_INTEGER64 && (val<=NA_INTEGER || val>INT_MAX),   val, _("%"PRId64" (type '%s') at RHS position %d out-of-range (NA) when assigning to type '%s' %s"))
              else  CHECK_RANGE(double, REAL,  !ISNAN(val) && (!within_int32_repres(val) || (int)val!=val),        val, _("%f (type '%s') at RHS position %d out-of-range(NA) or truncated (precision lost) when assigning to type '%s' %s"))
      case CPLXSXP: CHECK_RANGE(Rcomplex, COMPLEX, !((ISNAN(val.i) || (R_FINITE(val.i) && val.i==0.0)) &&
                                                     (ISNAN(val.r) || (within_int32_repres(val.r) && (int)val.r==val.r))), val.r, _("%f (type '%s') at RHS position %d either imaginary part discarded or real part truncated (precision lost) when assigning to type '%s' %s"))
      } break;
    case REALSXP:
      switch (TYPEOF(source)) {
      case REALSXP: if (targetIsI64 && !sourceIsI64)
                    CHECK_RANGE(double, REAL,  !ISNAN(val) && (!within_int64_repres(val) || (int64_t)val!=val),    val, _("%f (type '%s') at RHS position %d out-of-range(NA) or truncated (precision lost) when assigning to type '%s' %s"))
                    break;
      case CPLXSXP: if (targetIsI64)
                    CHECK_RANGE(Rcomplex, COMPLEX, !((ISNAN(val.i) || (R_FINITE(val.i) && val.i==0.0)) &&
                                                     (ISNAN(val.r) || (R_FINITE(val.r) && (int64_t)val.r==val.r))), val.r, _("%f (type '%s') at RHS position %d either imaginary part discarded or real part truncated (precision lost) when assigning to type '%s' %s"))
              else  CHECK_RANGE(Rcomplex, COMPLEX, !(ISNAN(val.i) || (R_FINITE(val.i) && val.i==0.0)),  val.r, _("%f (type '%s') at RHS position %d imaginary part discarded when assigning to type '%s' %s"))
      }
    }
  }

  #undef BODY
  #define BODY(STYPE, RFUN, CTYPE, CAST, ASSIGN) {{    \
    const STYPE *sd = (const STYPE *)RFUN(source);     \
    if (length(where)) {                               \
      if (slen==1) {                                   \
        const STYPE val = sd[soff];                    \
        const CTYPE cval = CAST;                       \
        for (int wi=0; wi<len; ++wi) {                 \
          const int w = wd[wi];                        \
          if (w<1) continue; /*0 or NA*/               \
          const int i = w-1;                           \
          ASSIGN;                                      \
        }                                              \
      } else {                                         \
        for (int wi=0; wi<len; ++wi) {                 \
          const int w = wd[wi];                        \
          if (w<1) continue;                           \
          const STYPE val = sd[wi+soff];               \
          const CTYPE cval = CAST;                     \
          const int i = w-1;                           \
          ASSIGN;                                      \
        }                                              \
      }                                                \
    } else {                                           \
      if (slen==1) {                                   \
        const STYPE val = sd[soff];                    \
        const CTYPE cval = CAST;                       \
        for (int i=0; i<len; ++i) {                    \
          ASSIGN;                                      \
        }                                              \
      } else {                                         \
        for (int i=0; i<len; i++) {                    \
          const STYPE val = sd[i+soff];                \
          const CTYPE cval = CAST;                     \
          ASSIGN;                                      \
        }                                              \
      }                                                \
    }                                                  \
  } break; }

  #define COERCE_ERROR(targetType) error(_("type '%s' cannot be coerced to '%s'"), type2char(TYPEOF(source)), targetType); // 'targetType' for integer64 vs double

  const int off = length(where) ? 0 : start;  // off = target offset; e.g. called from rbindlist with where=R_NilValue and start!=0
  const bool mc = length(where)==0 && slen>0 && slen==len && soff==0;  // mc=memcpy; only if types match and not for single items (a single assign faster than these non-const memcpy calls)
  const int *wd = length(where) ? INTEGER(where)+start : NULL;
  switch (TYPEOF(target)) {
  case RAWSXP: {
    Rbyte *td = RAW(target) + off;
    switch (TYPEOF(source)) {
    case RAWSXP:
      if (mc) {
                    memcpy(td, RAW_RO(source), slen*sizeof(Rbyte)); break;
      } else        BODY(Rbyte, RAW,    Rbyte, val,                                     td[i]=cval)
    case LGLSXP:    BODY(int, LOGICAL,  Rbyte, val==1,                                  td[i]=cval)
    case INTSXP:    BODY(int, INTEGER,  Rbyte, (val>255 || val<0) ? 0 : val,            td[i]=cval)
    case REALSXP:
      if (sourceIsI64)
                    BODY(int64_t, REAL, Rbyte, (val>255 || val<0) ? 0 : val,            td[i]=cval)
      else          BODY(double, REAL,  Rbyte, (ISNAN(val)||val>255||val<0) ? 0 : val,  td[i]=cval)
    default:        COERCE_ERROR("raw");
    }
  } break;
  case LGLSXP: {
    int *td = LOGICAL(target) + off;
    switch (TYPEOF(source)) {
    case RAWSXP:    BODY(Rbyte, RAW,    int, val!=0,                                    td[i]=cval)
    case LGLSXP:
      if (mc) {
                    memcpy(td, LOGICAL_RO(source), slen*sizeof(*td)); break;
      } else        BODY(int, LOGICAL,  int, val,                                       td[i]=cval)
    case INTSXP:    BODY(int, INTEGER,  int, val==NA_INTEGER ? NA_LOGICAL : val!=0,     td[i]=cval)
    case REALSXP:
      if (sourceIsI64)
                    BODY(int64_t, REAL, int, val==NA_INTEGER64 ? NA_LOGICAL : val!=0,   td[i]=cval)
      else          BODY(double,  REAL, int, ISNAN(val) ? NA_LOGICAL : val!=0.0,        td[i]=cval)
    default:        COERCE_ERROR("logical");
    }
  } break;
  case INTSXP : {
    int *td = INTEGER(target) + off;
    switch (TYPEOF(source)) {
    case  RAWSXP:   BODY(Rbyte, RAW,    int, (int)val,                                  td[i]=cval)
    case  LGLSXP:   // same as INTSXP ...
    case  INTSXP:
      if (mc) {
                    memcpy(td, INTEGER_RO(source), slen*sizeof(*td)); break;
      } else        BODY(int, INTEGER,  int, val,                                       td[i]=cval)
    case REALSXP:
      if (sourceIsI64)
                    BODY(int64_t, REAL, int, (val==NA_INTEGER64||val>INT_MAX||val<=NA_INTEGER) ? NA_INTEGER : (int)val,  td[i]=cval)
      else          BODY(double, REAL,  int, (ISNAN(val) || !within_int32_repres(val)) ? NA_INTEGER : (int)val,        td[i]=cval)
    case CPLXSXP:   BODY(Rcomplex, COMPLEX, int, (ISNAN(val.r) || !within_int32_repres(val.r)) ? NA_INTEGER : (int)val.r, td[i]=cval)
    default:        COERCE_ERROR("integer"); // test 2005.4
    }
  } break;
  case REALSXP : {
    if (targetIsI64) {
      int64_t *td = (int64_t *)REAL(target) + off;
      switch (TYPEOF(source)) {
      case RAWSXP:  BODY(Rbyte, RAW,    int64_t, (int64_t)val,                          td[i]=cval)
      case LGLSXP:  // same as INTSXP
      case INTSXP:  BODY(int, INTEGER,  int64_t, val==NA_INTEGER ? NA_INTEGER64 : val,  td[i]=cval)
      case REALSXP:
        if (sourceIsI64) {
          if (mc) {
                    memcpy(td, (const int64_t *)REAL_RO(source), slen*sizeof(*td)); break;
          } else    BODY(int64_t, REAL, int64_t, val,                                   td[i]=cval)
        } else      BODY(double, REAL,  int64_t, within_int64_repres(val) ? val : NA_INTEGER64,    td[i]=cval)
      case CPLXSXP: BODY(Rcomplex, COMPLEX, int64_t, ISNAN(val.r) ? NA_INTEGER64 : (int64_t)val.r, td[i]=cval)
      default:      COERCE_ERROR("integer64");
      }
    } else {
      double *td = REAL(target) + off;
      switch (TYPEOF(source)) {
      case  RAWSXP: BODY(Rbyte, RAW,    double, (double)val,                            td[i]=cval)
      case  LGLSXP: // same as INTSXP
      case  INTSXP: BODY(int, INTEGER,  double, val==NA_INTEGER ? NA_REAL : val,        td[i]=cval)
      case REALSXP:
        if (!sourceIsI64) {
          if (mc) {
                    memcpy(td, (const double *)REAL_RO(source), slen*sizeof(double)); break;
          } else    BODY(double, REAL,  double, val,                                    td[i]=cval)
        } else      BODY(int64_t, REAL, double, val==NA_INTEGER64 ? NA_REAL : val,      td[i]=cval)
      case CPLXSXP: BODY(Rcomplex, COMPLEX, double, val.r,                              td[i]=cval)
      default:      COERCE_ERROR("double");
      }
    }
  } break;
  case CPLXSXP: {
    Rcomplex *td = COMPLEX(target) + off;
    double im = 0.0;
    switch (TYPEOF(source)) {
    case  RAWSXP:   BODY(Rbyte, RAW,    double, (im=0.0,val),                                         td[i].r=cval;td[i].i=im)
    case  LGLSXP:   // same as INTSXP
    case  INTSXP:   BODY(int, INTEGER,  double, val==NA_INTEGER?(im=NA_REAL,NA_REAL):(im=0.0,val),    td[i].r=cval;td[i].i=im)
    case REALSXP:
      if (sourceIsI64)
                    BODY(int64_t, REAL, double, val==NA_INTEGER64?(im=NA_REAL,NA_REAL):(im=0.0,val),  td[i].r=cval;td[i].i=im)
      else          BODY(double,  REAL, double, ISNAN(val)?(im=NA_REAL,NA_REAL):(im=0.0,val),         td[i].r=cval;td[i].i=im)
    case CPLXSXP:
      if (mc) {
                    memcpy(td, COMPLEX_RO(source), slen*sizeof(Rcomplex)); break;
      } else        BODY(Rcomplex, COMPLEX, Rcomplex, val,                                            td[i]=cval)
    default:        COERCE_ERROR("complex");
    }
  } break;
  case STRSXP :
    if (sourceIsFactor) {
      const SEXP *ld = STRING_PTR_RO(PROTECT(getAttrib(source, R_LevelsSymbol))); protecti++;
      BODY(int, INTEGER, SEXP, val==NA_INTEGER ? NA_STRING : ld[val-1],  SET_STRING_ELT(target, off+i, cval))
    } else {
      if (!isString(source)) {
        if (allNA(source, true)) {  // saves common coercion of NA (logical) to NA_character_
          //              ^^ =errorForBadType; if type list, that was already an error earlier so we
          //                 want to be strict now otherwise list would get to coerceVector below
          if (length(where)) {
            for (int i=0; i<len; ++i) if (wd[i]>0) SET_STRING_ELT(target, wd[i]-1, NA_STRING);
          } else {
            for (int i=0; i<len; ++i) SET_STRING_ELT(target, start+i, NA_STRING);
          }
          break;
        }
        if (isObject(source)) {
          // otherwise coerceVector doesn't call the as.character method for Date, IDate, integer64, nanotime, etc; PR#5189
          // this if() is to save the overhead of the R call eval() when we know there can be no method
          source = PROTECT(eval(PROTECT(lang2(sym_as_character, source)), R_GlobalEnv)); protecti+=2;
        } else {
          source = PROTECT(coerceVector(source, STRSXP)); protecti++;
        }
      }
      BODY(SEXP, STRING_PTR_RO, SEXP, val,  SET_STRING_ELT(target, off+i, cval))
    }
  case VECSXP :
  case EXPRSXP : {  // #546 #4350
    if (len == 1 && TYPEOF(source)!=VECSXP && TYPEOF(source)!=EXPRSXP) {
        BODY(SEXP, &, SEXP, val, SET_VECTOR_ELT(target, off+i, cval))
    } else {
      switch (TYPEOF(source)) {
      // allocVector instead of ScalarLogical to avoid copyMostAttrib on R's internal global TRUE/FALSE values; #4595. Then because
      //   ScalarInteger may now or in future R also return R internal global small integer constants, the same for that. Then
      //   because we do that here for logical and integer, use allocVeector too for the other types to follow the same pattern and possibly
      //   in future R will also have some global constants for those types too.
      // the UNPROTECT can be at the end of the CAST before the SET_VECTOR_ELT, because SET_VECTOR_ELT will protect it and there's no other code in between
      // the PROTECT is now needed because of the call to LOGICAL() which could feasibly gc inside it.
      // copyMostAttrib is inside CAST so as to be outside loop.  See the history in #4350 and its follow up
      case RAWSXP:  BODY(Rbyte,    RAW,           SEXP, PROTECT(allocVector(RAWSXP, 1));RAW(cval)[0]=val;copyMostAttrib(source,cval);UNPROTECT(1),             SET_VECTOR_ELT(target,off+i,cval))
      case LGLSXP:  BODY(int,      LOGICAL,       SEXP, PROTECT(allocVector(LGLSXP, 1));LOGICAL(cval)[0]=val;copyMostAttrib(source,cval);UNPROTECT(1),         SET_VECTOR_ELT(target,off+i,cval))
      case INTSXP:  BODY(int,      INTEGER,       SEXP, PROTECT(allocVector(INTSXP, 1));INTEGER(cval)[0]=val;copyMostAttrib(source,cval);UNPROTECT(1),         SET_VECTOR_ELT(target,off+i,cval))
      case REALSXP: BODY(double,   REAL,          SEXP, PROTECT(allocVector(REALSXP, 1));REAL(cval)[0]=val;copyMostAttrib(source,cval);UNPROTECT(1),           SET_VECTOR_ELT(target,off+i,cval))
      case CPLXSXP: BODY(Rcomplex, COMPLEX,       SEXP, PROTECT(allocVector(CPLXSXP, 1));COMPLEX(cval)[0]=val;copyMostAttrib(source,cval);UNPROTECT(1),        SET_VECTOR_ELT(target,off+i,cval))
      case STRSXP:  BODY(SEXP,     STRING_PTR_RO, SEXP, PROTECT(allocVector(STRSXP, 1));SET_STRING_ELT(cval, 0, val);copyMostAttrib(source,cval);UNPROTECT(1), SET_VECTOR_ELT(target,off+i,cval))
      case VECSXP:
      case EXPRSXP: BODY(SEXP,     SEXPPTR_RO, SEXP, val,                                                                                                   SET_VECTOR_ELT(target,off+i,cval))
      default: COERCE_ERROR("list");
      }
    }
  } break;
  default : // # nocov
    error(_("Unsupported column type in assign.c:memrecycle '%s'"), type2char(TYPEOF(target)));  // # nocov
  }
  UNPROTECT(protecti);
  return memrecycle_message[0] ? memrecycle_message : NULL;
}

void writeNA(SEXP v, const int from, const int n, const bool listNA)
// e.g. for use after allocVector() which does not initialize its result.
// listNA for #5503
{
  const int to = from-1+n;  // writing to position 2147483647 in mind, 'i<=to' in loop conditions
  switch(TYPEOF(v)) {
  case RAWSXP:
    memset(RAW(v)+from, 0, n*sizeof(Rbyte));
    break;
  case LGLSXP: {
    int *vd = LOGICAL(v);
    for (int i=from; i<=to; ++i) vd[i] = NA_LOGICAL;
  } break;
  case INTSXP: {
    // same whether factor or not
    int *vd = INTEGER(v);
    for (int i=from; i<=to; ++i) vd[i] = NA_INTEGER;
  } break;
  case REALSXP: {
    if (INHERITS(v, char_integer64)) {
      int64_t *vd = (int64_t *)REAL(v);
      for (int i=from; i<=to; ++i) vd[i] = NA_INTEGER64;
    } else {
      double *vd = REAL(v);
      for (int i=from; i<=to; ++i) vd[i] = NA_REAL;
    }
  } break;
  case CPLXSXP: {
    Rcomplex *vd = COMPLEX(v);
    for (int i=from; i<=to; ++i) vd[i] = NA_CPLX;
  } break;
  case STRSXP:
    // character columns are initialized with blank string (""). So replace the all-"" with all-NA_character_
    // Since "" and NA_character_ are global constants in R, it should be ok to not use SET_STRING_ELT here. But use it anyway for safety (revisit if proved slow)
    // If there's ever a way added to R API to pass NA_STRING to allocVector() to tell it to initialize with NA not "", would be great
    for (int i=from; i<=to; ++i) SET_STRING_ELT(v, i, NA_STRING);
    break;
  case VECSXP: {
    // See #5053 for comments and discussion re listNA
    // although allocVector initializes to R_NilValue, we use writeNA() in other places too, so we shouldn't skip the R_NilValue assign
    // ScalarLogical(NA_LOGICAL) returns R's internal constant R_LogicalNAValue (no alloc and no protect needed)
    const SEXP na = listNA ? ScalarLogical(NA_LOGICAL) : R_NilValue;
    for (int i=from; i<=to; ++i) SET_VECTOR_ELT(v, i, na);
  } break;
  case EXPRSXP :
    for (int i=from; i<=to; ++i) SET_VECTOR_ELT(v, i, R_NilValue);
    break;
  default : // # nocov
    internal_error(__func__, "Unsupported type '%s' for v", type2char(TYPEOF(v)));  // # nocov
  }
}

SEXP allocNAVector(SEXPTYPE type, R_len_t n)
{
  // an allocVector following with initialization to NA since a subassign to a new column using :=
  // routinely leaves untouched items (rather than 0 or "" as allocVector does with its memset)
  // We guess that author of allocVector would have liked to initialize with NA but was prevented since memset
  // is restricted to one byte.
  SEXP v = PROTECT(allocVector(type, n));
  writeNA(v, 0, n, false);
  UNPROTECT(1);
  return(v);
}

SEXP allocNAVectorLike(SEXP x, R_len_t n) {
  // writeNA needs the attribute retained to write NA_INTEGER64, #3723
  // TODO: remove allocNAVector above when usage in fastmean.c, fcast.c and fmelt.c can be adjusted; see comments in PR3724
  SEXP v = PROTECT(allocVector(TYPEOF(x), n));
  copyMostAttrib(x, v);
  writeNA(v, 0, n, false);
  UNPROTECT(1);
  return(v);
}

static SEXP *saveds=NULL;
static R_len_t *savedtl=NULL, nalloc=0, nsaved=0;

void savetl_init(void) {
  if (nsaved || nalloc || saveds || savedtl) {
    internal_error(__func__, "savetl_init checks failed (%d %d %p %p)", nsaved, nalloc, (void *)saveds, (void *)savedtl); // # nocov
  }
  nsaved = 0;
  nalloc = 100;
  saveds = malloc(sizeof(*saveds) * nalloc);
  savedtl = malloc(sizeof(*savedtl) * nalloc);
  if (!saveds || !savedtl) {
    free(saveds); free(savedtl);                                            // # nocov
    savetl_end();                                                           // # nocov
    error(_("Failed to allocate initial %d items in savetl_init"), nalloc); // # nocov
  }
}

void savetl(SEXP s)
{
  if (nsaved==nalloc) {
    if (nalloc==INT_MAX) {
      savetl_end();                                                                                                     // # nocov
      internal_error(__func__, "reached maximum %d items for savetl", nalloc); // # nocov
    }
    nalloc = nalloc>(INT_MAX/2) ? INT_MAX : nalloc*2;
    char *tmp = realloc(saveds, sizeof(SEXP)*nalloc);
    if (tmp==NULL) {
      // C spec states that if realloc() fails the original block is left untouched; it is not freed or moved. We rely on that here.
      savetl_end();                                                      // # nocov  free(saveds) happens inside savetl_end
      error(_("Failed to realloc saveds to %d items in savetl"), nalloc);   // # nocov
    }
    saveds = (SEXP *)tmp;
    tmp = realloc(savedtl, sizeof(R_len_t)*nalloc);
    if (tmp==NULL) {
      savetl_end();                                                      // # nocov
      error(_("Failed to realloc savedtl to %d items in savetl"), nalloc);  // # nocov
    }
    savedtl = (R_len_t *)tmp;
  }
  saveds[nsaved] = s;
  savedtl[nsaved] = TRUELENGTH(s);
  nsaved++;
}

void savetl_end(void) {
  // Can get called if nothing has been saved yet (nsaved==0), or even if _init() hasn't been called yet (pointers NULL). Such
  // as to clear up before error. Also, it might be that nothing needed to be saved anyway.
  for (int i=0; i<nsaved; i++) SET_TRUELENGTH(saveds[i],savedtl[i]);
  free(saveds);  // possible free(NULL) which is safe no-op
  saveds = NULL;
  free(savedtl);
  savedtl = NULL;
  nsaved = nalloc = 0;
}

SEXP setcharvec(SEXP x, SEXP which, SEXP newx)
{
  int w;
  if (!isString(x)) error(_("x must be a character vector"));
  if (!isInteger(which)) error(_("'which' must be an integer vector"));
  if (!isString(newx)) error(_("'new' must be a character vector"));
  if (LENGTH(newx)!=LENGTH(which)) error(_("'new' is length %d. Should be the same as length of 'which' (%d)"),LENGTH(newx),LENGTH(which));
  for (int i=0; i<LENGTH(which); i++) {
    w = INTEGER(which)[i];
    if (w==NA_INTEGER || w<1 || w>LENGTH(x)) error(_("Item %d of 'which' is %d which is outside range of the length %d character vector"), i+1,w,LENGTH(x));
    SET_STRING_ELT(x, w-1, STRING_ELT(newx, i));
  }
  return R_NilValue;
}
