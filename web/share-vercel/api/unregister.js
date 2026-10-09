// POST /api/unregister: the Hub stopped sharing.
import { handleUnregister } from "../lib/handlers.js";

export default { fetch: handleUnregister };
