// GET /api/resolve/<token>: where the listen / send page has to connect right now.
import { handleResolve } from "../../lib/handlers.js";

export default { fetch: handleResolve };
