#pragma once

// The subset of MegaApi's USER_ATTR_* values this app reads. Parameterises
// getMyUserAttribute rather than giving each attribute its own port method, so the
// "join first + last, degrade to whatever arrived" policy stays in AccountService,
// which a mock can test -- the SDK adapter has no test, needing a live account.
enum class UserAttribute
{
    FirstName,
    LastName,
};
