// Variable test stand-in for RRFLibraries General/function_ref.h, as far as Variable.h uses it
#ifndef VARIABLE_TEST_FUNCTION_REF_H
#define VARIABLE_TEST_FUNCTION_REF_H

#include <functional>

template<typename F> class function_ref;
template<typename R, typename... Args> class function_ref<R(Args...)>
{
public:
	template<typename C> function_ref(C&& c) noexcept : f(std::forward<C>(c)) { }
	R operator()(Args... args) const { return f(std::forward<Args>(args)...); }
private:
	std::function<R(Args...)> f;
};

template<typename F> class function_ref_noexcept;
template<typename R, typename... Args> class function_ref_noexcept<R(Args...) noexcept>
{
public:
	template<typename C> function_ref_noexcept(C&& c) noexcept : f(std::forward<C>(c)) { }
	R operator()(Args... args) const noexcept { return f(std::forward<Args>(args)...); }
private:
	std::function<R(Args...)> f;
};

#endif
