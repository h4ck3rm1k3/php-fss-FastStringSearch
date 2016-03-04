/*
 * PHP extension for fast string search routines
 * Copyright Tim Starling, 2006; Aaron Schulz 2014
 * License: DWTFYWWI
*/

#include "hphp/runtime/ext/extension.h"
#include "hphp/runtime/base/string-buffer.h"
#include "hphp/runtime/base/array-init.h"

#include "kwset.h"

namespace HPHP {

typedef struct {
	kwset_t set;
	int replace_size;
	Variant* replace[1];
} fss_resource_t;

class FastStringSearchResource : public SweepableResourceData {
public:
	DECLARE_RESOURCE_ALLOCATION(FastStringSearchResource)
	CLASSNAME_IS("fss")
	virtual const String& o_getClassNameHook() const {
		return classnameof();
	}

	explicit FastStringSearchResource(fss_resource_t* fss_r) {
		m_fss_r = fss_r;
	}
	virtual ~FastStringSearchResource() {
		close();
	}
	void close() {
		sweep();
		for (int i = 0; i < m_fss_r->replace_size; i++) {
			if (!m_fss_r->replace[i]) {
				continue;
			}
			req::destroy_raw(m_fss_r->replace[i]);
			m_fss_r->replace[i] = NULL;
		}
		req::free(m_fss_r);
		m_fss_r = NULL;
	}
	fss_resource_t* getStruct() {
		return m_fss_r;
	}
private:
	fss_resource_t* m_fss_r;
};

void FastStringSearchResource::sweep() {
	kwsfree(m_fss_r->set);
}

static Variant HHVM_FUNCTION(fss_prep_search, const Variant& needle) {
	fss_resource_t * res;
	const char *error;

	res = (fss_resource_t*)req::malloc(sizeof(fss_resource_t));
	if (!res) {
		throw Exception("Out of memory");
	}

	res->set = kwsalloc(NULL);
	if (!res->set) {
		throw Exception("Out of memory");
	}

	res->replace_size = 0;

	if (needle.getType() == KindOfArray) {
		Array needle_a = needle.toArray();
		for(ArrayIter iter(needle_a); iter; ++iter) {
			Variant key = iter.first();
			String elem = iter.second().toString();
			/* Don't add zero-length strings, that will cause infinite loops in
				search routines */
			if (!elem.size()) {
				continue;
			}
			error = kwsincr(res->set, elem.c_str(), elem.size());
			if (error) {
				raise_warning("fss_prep_search: %s", error);
				return false;
			}
		}
	} else {
		String needle_s = needle.toString();
		error = kwsincr(res->set, needle_s.c_str(), needle_s.size());
		if (error) {
			raise_warning("fss_prep_search: %s", error);
			return false;
		}
	}
	error = kwsprep(res->set);
	if (error) {
		raise_warning("kwsprep: %s", error);
		return false;
	}

	FastStringSearchResource * ret_res = HPHP::req::make_raw<FastStringSearchResource>(res);
	const req::ptr<FastStringSearchResource> ret_ptr(ret_res);
	HPHP::Variant r (ret_ptr);
	    
	return r;
}

static Variant HHVM_FUNCTION(fss_exec_search, const Resource& r, const String& haystack, int64_t offset ) {
  FastStringSearchResource * fss_r = (FastStringSearchResource * )r.hdr();
    //r.getTyped<>();
	if (!fss_r) {
		return false;
	}

	fss_resource_t * res = fss_r->getStruct();
	struct kwsmatch m;
	size_t match_pos;

	if (offset >= haystack.size() || offset < 0) {
		return false;
	}

	match_pos = kwsexec(res->set, haystack.c_str() + offset, haystack.size() - offset, &m);
	if (match_pos == (size_t)-1) {
		return false;
	}

	Array match = make_packed_array((int64_t)(m.offset[0] + offset), (int64_t)(m.size[0]));

	return match;
}

static Resource HHVM_FUNCTION(fss_prep_replace, const Array& pairs ) {
	fss_resource_t * res;
	const char *error;
	int i = 0;
	const char *string_key;
	size_t string_key_len;
	char buffer[40];

	/* fss_resource_t has an open-ended array, we allocate enough memory for the
	   header plus all the array elements, plus one extra element for good measure */
	res = (fss_resource_t*)req::malloc(sizeof(Variant*) * pairs.size() + sizeof(fss_resource_t));
	if (!res) {
		throw Exception("Out of memory");
	}

	res->set = kwsalloc(NULL);
	if (!res->set) {
		throw Exception("Out of memory");
	}
	res->replace_size = pairs.size();

	for(ArrayIter iter(pairs); iter; ++iter) {
		String s;
		Variant key = iter.first();
		/* Convert numeric keys to string */
		if(key.getType() == KindOfInt64) {
			snprintf(buffer, sizeof(buffer), "%lu", key.toInt64());
			string_key = buffer;
			string_key_len = strlen(buffer);
		} else {
			s = key.toString();
			string_key = s.c_str();
			string_key_len = s.size();
		}

		/* Don't add zero-length strings, that will cause infinite loops in
		   search routines */
		if (!string_key_len) {
			res->replace[i] = NULL;
			continue;
		}

		/* Add the key to the search tree */
		error = kwsincr(res->set, string_key, string_key_len);
		if (error) {
			res->replace[i] = NULL;
			raise_warning("fss_prep_replace: %s", error);
			continue;
		}

		/* Add the value to the replace array */
		res->replace[i++] = HPHP::req::make_raw<Variant>(iter.second());
	}
	for (; i < res->replace_size; i++) {
		res->replace[i] = NULL;
	}
	error = kwsprep(res->set);
	if (error) {
		throw Exception("kwsprep: %s", error);
	}

	FastStringSearchResource * ret_res = HPHP::req::make_raw<FastStringSearchResource>(res);
	const req::ptr<FastStringSearchResource> ret_ptr(ret_res);
	HPHP::Resource r (ret_ptr);
	return r;
}

static Variant HHVM_FUNCTION(fss_exec_replace, const Resource& r, const String& subject) {
  //auto fss_r = r.getTyped<FastStringSearchResource>();
    FastStringSearchResource * fss_r = (FastStringSearchResource * )r.hdr();
	if (!fss_r) {
		return false;
	}

	fss_resource_t * res = fss_r->getStruct();
	size_t match_pos = 0;
	struct kwsmatch m;
	StringBuffer result;
	const char * subject_ptr = subject.c_str();
	int subject_len = subject.size();

	while (subject_len > 0 &&
		(size_t)-1 != (match_pos = kwsexec(res->set, subject_ptr, subject_len, &m)))
	{
		/* Output the leading portion */
		result.append(subject_ptr, match_pos);

		/* Output the replacement portion
		   The index may be above the size of the replacement array if the
		   object was prepared as a search object instead of a replacement
		   object. In that case, we replace the item with an empty string
		 */
		if (m.index < res->replace_size) {
			Variant* temp = res->replace[m.index];
			if (temp) {
				String temp_s = temp->toString();
				result.append(temp_s.c_str(), temp_s.size());
			}
		}

		/* Increment and continue */
		subject_len -= match_pos + m.size[0];
		subject_ptr += match_pos + m.size[0];
	}
	/* Output the remaining portion */
	if (subject_len > 0) {
		result.append(subject_ptr, subject_len);
	}
	/* Return the result */
	return result.detach();
}

static bool HHVM_FUNCTION(fss_free, const Resource& r) {
	return true;
}

static class FSSExtension : public Extension {
	public:
	FSSExtension() : Extension("fss") {}
	virtual void moduleInit() {
		HHVM_FE(fss_prep_search);
		HHVM_FE(fss_exec_search);
		HHVM_FE(fss_prep_replace);
		HHVM_FE(fss_exec_replace);
		HHVM_FE(fss_free);
		loadSystemlib();
	}
} s_fss_extension;

HHVM_GET_MODULE(fss)

} // namespace HPHP
