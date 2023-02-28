/**
 * Copyright (c) 2022, NVIDIA CORPORATION. All rights reserved.
 *
 * See file LICENSE for terms.
 */
#pragma once

#include <atomic>
#include <chrono>

#include <ucp/api/ucp.h>

#include <ucxx/component.h>
#include <ucxx/endpoint.h>
#include <ucxx/future.h>
#include <ucxx/typedefs.h>

#define ucxx_trace_req_f(_owner, _req, _name, _message, ...) \
  ucxx_trace_req("%s, req %p, op %s: " _message, (_owner), (_req), (_name), ##__VA_ARGS__)

namespace ucxx {

class Request : public Component {
 protected:
  std::atomic<ucs_status_t> _status{UCS_INPROGRESS};  ///< Requests status
  std::string _status_msg{};                          ///< Human-readable status message
  void* _request{nullptr};                            ///< Pointer to UCP request
  std::shared_ptr<Future> _future{nullptr};           ///< Future to notify upon completion
  std::function<void(std::shared_ptr<void>)> _callback{nullptr};  ///< Completion callback
  std::shared_ptr<void> _callbackData{nullptr};                   ///< Completion callback data
  std::shared_ptr<Worker> _worker{
    nullptr};  ///< Worker that generated request (if not from endpoint)
  std::shared_ptr<Endpoint> _endpoint{
    nullptr};  ///< Endpoint that generated request (if not from worker)
  std::string _ownerString{
    "undetermined owner"};  ///< String to print owner (endpoint or worker) when logging
  std::shared_ptr<DelayedSubmission> _delayedSubmission{
    nullptr};  ///< The submission object that will dispatch the request
  std::string _operationName{
    "request_undefined"};          ///< Human-readable operation name, mostly used for log messages
  bool _enablePythonFuture{true};  ///< Whether Python future is enabled for this request

  /**
   * @brief Protected constructor of an abstract `ucxx::Request`.
   *
   * Construct an abstract request, registering the request to the appropriate parent
   * (either an endpoint or a worker) so that it may be canceled if necessary. This
   * constuctor is also responsible for populating a `ucxx::DelayedSubmission` object that
   * will effectively dispatch the request when appropriate (either immediately or a the
   * next worker progress iteration), as well as create Python futures that can be later
   * awaited in Python asynchronous code.
   *
   * @param[in] endpointOrWorker    the parent component, which may either be a
   *                                `std::shared_ptr<Endpoint>` or
   *                                `std::shared_ptr<Worker>`.
   * @param[in] delayedSubmission   the object to manage request submission.
   * @param[in] operationName       a human-readable operation name to help identifying
   *                                requests by their types when UCXX logging is enabled.
   * @param[in] enablePythonFuture  whether a python future should be created and
   *                                subsequently notified.
   */
  Request(std::shared_ptr<Component> endpointOrWorker,
          std::shared_ptr<DelayedSubmission> delayedSubmission,
          const std::string operationName,
          const bool enablePythonFuture = false);

  /**
   * @brief Perform initial processing of the request to determine if immediate completion.
   *
   * Perform initial processing of the requeste, determining whether it completed
   * immediately, in which case it will set its status and call a user-defined callback
   * (when registered by the derived class), otherwise do nothing until the UCX operation
   * completes and the internal callback is executed to finally set its state.
   */
  void process();

  /**
   * @brief Set the request status and notify Python future.
   *
   * Set the request status and notify the Python future of the status change if the
   * object was created with Python support. Intended to be called only once when the
   * request status changes to its final state.
   *
   * @param[in] status the status of the request to be set.
   */
  void setStatus(ucs_status_t status);

 public:
  Request()               = delete;
  Request(const Request&) = delete;
  Request& operator=(Request const&) = delete;
  Request(Request&& o)               = delete;
  Request& operator=(Request&& o) = delete;

  /**
   * @brief `ucxx::Request` destructor.
   *
   * Removes its own reference from its parent's inflight messages collection and
   * free internal resources.
   */
  virtual ~Request();

  /**
   * @brief Cancel the request.
   *
   * Cancel the request. Often called by the an error handler or parent's object
   * destructor but may be called by the user to cancel the request as well.
   */
  void cancel();

  /**
   * @brief Return the status of the request.
   *
   * Return a `ucs_status_t` that may be used for more fine-grained error handling than
   * relying on `checkError()` alone, which does not currently implement all error
   * statuses supported by UCX.
   *
   * @return the current status of the request.
   */
  ucs_status_t getStatus();

  /**
   * @brief Return the future used to check on state.
   *
   * If the object has enabled Python future support, return the future that can be
   * awaited from Python, returns `nullptr` otherwise.
   *
   * @returns the Python future object or `nullptr`.
   */
  void* getFuture();

  /**
   * @brief Check whether the request completed with an error.
   *
   * Check whether the request has completed with an error, if an error occurred an
   * exception is raised, but if the request has completed or is in progress this call will
   * act as a no-op. To verify whether the request is in progress either `isCompleted()` or
   * `getStatus()` should be checked.
   *
   * @throw `ucxx::CanceledError`         if the request was canceled.
   * @throw `ucxx::MessageTruncatedError` if the message was truncated.
   * @throw `ucxx::Error`                 if another error occurred.
   */
  void checkError();

  /**
   * @brief Check whether the request has already completed.
   *
   * Check whether the request has already completed. The status of the request must be
   * verified with `getStatus()` before consumption.
   *
   * @return whether the request has completed.
   */
  bool isCompleted();

  /**
   * @brief Callback executed by UCX when request is completed.
   *
   * Generic callback executed by UCX when a request is completed, used to set the status
   * of the request and free any resources associated with it.
   *
   * WARNING: This is not intended to be called by the user, but it currently needs to be
   * a public method so that UCX may access it. In future changes this will be moved to
   * an internal object and remove this method from the public API.
   *
   * @param[in] request the UCX request pointer.
   * @param[in] status  the completion status of the request.
   */
  void callback(void* request, ucs_status_t status);

  /**
   * @brief Populate the internal submission dispatcher.
   *
   * The `ucxx::Request` utilizes `ucxx::DelayedSubmission` to manage when the request will
   * be dispatched. This method is registered as a callback in the worker, that may choose
   * to either execute (submit) it immediately or delay for the next iteration of its
   * progress loop, depending on the progress mode in use by the worker.
   *
   * See `ucxx::DelayedSubmission::DelayedSubmission()` for more details.
   */
  virtual void populateDelayedSubmission() = 0;

  /**
   * @brief Get formatted string with owner type and handle address.
   *
   * Get a formatted string with owner type (worker or endpoint) and its respective handle
   * address. This is meant to get logging information for a request's callback, which is
   * not a member attribute of `ucxx::Request` or derived class, but a static method
   * or external function instead.
   *
   * @returns the formatted string containing the owner type and its handle.
   */
  const std::string& getOwnerString() const;
};

}  // namespace ucxx
