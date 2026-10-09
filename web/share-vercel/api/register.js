// POST /api/register: the Hub announces (and every 30 s renews) where its tunnel is.
import { handleRegister } from "../lib/handlers.js";

export default { fetch: handleRegister };
