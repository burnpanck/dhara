//
// Created by Yves Delley on 04.09.20.
//

#ifndef DHARA_ASYNC_H_
#define DHARA_ASYNC_H_

#include "error.hpp"

typedef void (*dhara_async_callback)(error_t error, void *stack_ptr);

//! Encapsulates persisted state through an asynchroneous procedure.
/*!
 * The procedure may use the space between stack_ptr and stack_end
 * to store internal procedure state. The procedure shall abort with
 * an error if more space would be required. The documentation should
 * clearly indicate the amount of state needed.
 *
 * Once the procedures completes, the provided callback is called with
 * the status code and the provided stack pointer as argument.
 */
struct dhara_async_call {
	dhara_async_callback callback;
	void *stack_ptr;
	const void *stack_end;
};

#endif //DHARA_ASYNC_H_
