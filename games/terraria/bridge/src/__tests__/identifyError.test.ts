import assert from 'node:assert/strict';
import { test } from 'node:test';
import { identifyErrorSummary, identifyErrorText } from '../takaro/identifyError.js';

test('a rejected identify is logged by name, message and status, never with its request', () => {
  const payload = {
    name: 'AxiosError',
    message: 'Request failed with status code 409',
    status: 409,
    config: { headers: { 'x-takaro-token': 'secret-jwt' } },
  };
  assert.equal(identifyErrorSummary(payload), 'AxiosError: Request failed with status code 409 (http 409)');
  assert.doesNotMatch(identifyErrorSummary(payload), /secret-jwt/);
  assert.equal(identifyErrorText(payload), 'Request failed with status code 409');
  assert.equal(identifyErrorSummary({ name: 'BadRequestError', message: 'Invalid registrationToken provided', http: 400 }),
    'BadRequestError: Invalid registrationToken provided (http 400)');
  assert.doesNotMatch(identifyErrorText({ config: { token: 'secret-jwt' } }), /secret-jwt/);
});
